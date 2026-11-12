/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui/mapping-editor.hpp"

#include "platform/platform.hpp"
#include "ui/preview-view.hpp"
#include "ui/theme.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace om {

enum ItemRole { RoleKind = Qt::UserRole + 1, RoleId };
enum ItemKind { KindDisplay = 1, KindSlice = 2 };

static QIcon dotIcon(const QColor &c, bool square = false)
{
	QPixmap pm(28, 28);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	if (square)
		p.drawRoundedRect(QRectF(5, 5, 18, 18), 4, 4);
	else
		p.drawEllipse(QRectF(8, 8, 12, 12));
	return QIcon(pm);
}

static QColor statusColor(const DisplayStatus &st, DisplayKind kind)
{
	if (kind == DisplayKind::Virtual)
		return QColor(theme::kMuted);
	switch (st.match) {
	case MatchStatus::Connected:
	case MatchStatus::ConnectedSerial:
		return QColor(theme::kOk);
	case MatchStatus::Ambiguous:
		return QColor(theme::kWarn);
	case MatchStatus::Disconnected:
		return QColor(theme::kLive);
	default:
		return QColor(theme::kMuted);
	}
}

/* Short badge for the sidebar's second column. */
static QString statusBadge(const DisplayStatus &st, DisplayKind kind, OutputMode m, QColor &color)
{
	if (m == OutputMode::Fullscreen) {
		color = QColor(theme::kLive);
		return QStringLiteral("● LIVE");
	}
	if (m == OutputMode::Window) {
		color = QColor(theme::kAccent);
		return QStringLiteral("WINDOW");
	}
	if (kind == DisplayKind::Virtual) {
		color = QColor(theme::kMuted);
		return QStringLiteral("VIRTUAL");
	}
	switch (st.match) {
	case MatchStatus::Ambiguous:
		color = QColor(theme::kWarn);
		return QStringLiteral("CHECK");
	case MatchStatus::Disconnected:
		color = QColor(theme::kLive);
		return QStringLiteral("OFFLINE");
	case MatchStatus::Unassigned:
		color = QColor(theme::kMuted);
		return QStringLiteral("UNSET");
	default:
		color = QColor(theme::kOk);
		return QStringLiteral("READY");
	}
}

/* ---------------------------------------------------------------------- */
/* SliceTree                                                               */

SliceTree::SliceTree(QWidget *parent) : QTreeWidget(parent)
{
	setHeaderHidden(true);
	setColumnCount(2);
	header()->setStretchLastSection(false);
	header()->setSectionResizeMode(0, QHeaderView::Stretch);
	header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setSelectionMode(QAbstractItemView::SingleSelection);
	setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	setContextMenuPolicy(Qt::CustomContextMenu);
	setIndentation(16);
	setIconSize(QSize(14, 14));
	setAnimated(true);
}

void SliceTree::dragMoveEvent(QDragMoveEvent *e)
{
	QTreeWidgetItem *dragged = currentItem();
	if (!dragged || dragged->data(0, RoleKind).toInt() != KindSlice) {
		e->ignore();
		return;
	}
	QTreeWidget::dragMoveEvent(e);
}

void SliceTree::dropEvent(QDropEvent *e)
{
	/* Translate the drop into a model operation; the tree is rebuilt from
	 * the model afterwards, so Qt never moves items itself. */
	QTreeWidgetItem *dragged = currentItem();
	e->setDropAction(Qt::IgnoreAction);
	e->accept();
	if (!dragged || dragged->data(0, RoleKind).toInt() != KindSlice)
		return;
	QString sliceId = dragged->data(0, RoleId).toString();

	QTreeWidgetItem *target = itemAt(e->position().toPoint());
	if (!target)
		return;
	QString displayId;
	int index = 0;
	DropIndicatorPosition pos = dropIndicatorPosition();
	if (target->data(0, RoleKind).toInt() == KindDisplay) {
		/* Dropped on a display row: put the slice on top of that display. */
		displayId = target->data(0, RoleId).toString();
		index = 0;
	} else {
		QTreeWidgetItem *parentItem = target->parent();
		displayId = parentItem->data(0, RoleId).toString();
		index = parentItem->indexOfChild(target);
		if (pos == QAbstractItemView::BelowItem)
			index++;
		if (dragged->parent() == parentItem && parentItem->indexOfChild(dragged) < index)
			index--;
	}
	QMetaObject::invokeMethod(
		this, [sliceId, displayId, index]() { Controller::instance()->moveSlice(sliceId, displayId, index); },
		Qt::QueuedConnection);
}

/* ---------------------------------------------------------------------- */
/* MappingEditor                                                           */

MappingEditor::MappingEditor(QWidget *parent) : QDialog(parent), c_(Controller::instance())
{
	setObjectName("OutputMappingEditor");
	setWindowTitle(QStringLiteral("Output Mapping"));
	setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setStyleSheet(theme::styleSheet());
	resize(1500, 860);
	setMinimumSize(1040, 600);

	buildUi();

	connect(c_, &Controller::documentChanged, this, [this](Change kind) {
		if (kind == Change::Structure) {
			rebuildTree();
			rebuildScreenCombo();
		}
		updateInspector();
		updateHeaders();
		updateActions();
	});
	connect(c_, &Controller::selectionChanged, this, [this]() {
		syncTreeSelection();
		rebuildScreenCombo();
		updateInspector();
		updateHeaders();
		updateActions();
	});
	connect(c_, &Controller::screensChanged, this, [this]() {
		rebuildTree();
		rebuildScreenCombo();
		updateInspector();
		updateHeaders();
	});
	connect(c_, &Controller::outputsChanged, this, [this]() {
		rebuildTree();
		updateInspector();
		updateHeaders();
		updateStatusBar();
	});
	connect(c_, &Controller::programSizeChanged, this, [this]() {
		updateHeaders();
		updateStatusBar();
	});
	connect(c_, &Controller::undoStateChanged, this, &MappingEditor::updateActions);

	rebuildTree();
	rebuildScreenCombo();
	updateInspector();
	updateHeaders();
	updateActions();
	updateStatusBar();
}

MappingEditor::~MappingEditor() = default;

void MappingEditor::buildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	/* --- toolbar --- */
	auto *tb = new QToolBar(this);
	tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

	addDisplayBtn_ = new QToolButton(tb);
	addDisplayBtn_->setObjectName("MenuButton");
	addDisplayBtn_->setText(QStringLiteral("＋ Display ▾"));
	addDisplayBtn_->setToolTip(QStringLiteral("Add a connected display, or a virtual one for testing"));
	addDisplayBtn_->setPopupMode(QToolButton::InstantPopup);
	auto *addMenu = new QMenu(addDisplayBtn_);
	connect(addMenu, &QMenu::aboutToShow, this, [this, addMenu]() { populateAddDisplayMenu(addMenu); });
	addDisplayBtn_->setMenu(addMenu);
	tb->addWidget(addDisplayBtn_);

	addSliceAct_ = tb->addAction(QStringLiteral("＋ Slice"), this, &MappingEditor::addSliceToSelected);
	addSliceAct_->setToolTip(QStringLiteral("Add a slice to the selected display (or just drag on Input Selection)"));
	tb->addSeparator();

	undoAct_ = tb->addAction(QStringLiteral("Undo"), this, [this]() { c_->undo(); });
	undoAct_->setShortcut(QKeySequence::Undo);
	undoAct_->setToolTip(
		QStringLiteral("Undo (%1)").arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)));
	redoAct_ = tb->addAction(QStringLiteral("Redo"), this, [this]() { c_->redo(); });
	redoAct_->setShortcut(QKeySequence::Redo);
	redoAct_->setToolTip(
		QStringLiteral("Redo (%1)").arg(QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText)));
	tb->addSeparator();

	dupAct_ = tb->addAction(QStringLiteral("Duplicate"), this, [this]() { c_->duplicateSlice(c_->selectedSlice()); });
	dupAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
	delAct_ = tb->addAction(QStringLiteral("Delete"), this, [this]() { c_->deleteSlice(c_->selectedSlice()); });
	tb->addSeparator();
	fitAct_ = tb->addAction(QStringLiteral("Fit to Display"), this,
				[this]() { c_->fitSliceToDisplay(c_->selectedSlice()); });
	fitAct_->setToolTip(QStringLiteral("Scale the output to fill the display, keeping the source's aspect ratio"));
	oneAct_ = tb->addAction(QStringLiteral("1:1 Pixels"), this, [this]() { c_->sliceOneToOne(c_->selectedSlice()); });
	oneAct_->setToolTip(QStringLiteral("Make the output exactly the source size (pixel for pixel)"));
	tb->addSeparator();
	snapAct_ = tb->addAction(QStringLiteral("Snap"));
	snapAct_->setCheckable(true);
	snapAct_->setChecked(true);
	snapAct_->setToolTip(QStringLiteral("Snap to edges, centre and other slices (hold %1 to bypass)").arg(altKeyName()));
	lockAct_ = tb->addAction(QStringLiteral("Lock Aspect"));
	lockAct_->setCheckable(true);
	lockAct_->setToolTip(QStringLiteral("Keep aspect ratio while resizing (hold Shift to toggle temporarily)"));

	auto *spacer = new QWidget(tb);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	tb->addWidget(spacer);

	auto *optBtn = new QToolButton(tb);
	optBtn->setObjectName("MenuButton");
	optBtn->setText(QStringLiteral("Options ▾"));
	optBtn->setPopupMode(QToolButton::InstantPopup);
	auto *optMenu = new QMenu(optBtn);
	restoreAct_ = optMenu->addAction(QStringLiteral("Restore running outputs when OBS starts"));
	restoreAct_->setCheckable(true);
	captureAct_ = optMenu->addAction(QStringLiteral("Hide outputs from screen capture (prevents feedback)"));
	captureAct_->setCheckable(true);
	optMenu->setToolTipsVisible(true);
	restoreAct_->setToolTip(QStringLiteral("Only onto displays identified exactly, never onto another screen"));
	captureAct_->setToolTip(QStringLiteral("Display/window capture can't see the outputs, so Program can never "
					       "contain its own output"));
	optMenu->addSeparator();
	optMenu->addAction(QStringLiteral("Open Settings Folder"), this, [this]() {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(c_->configPath()).absolutePath()));
	});
	connect(optMenu, &QMenu::aboutToShow, this, [this]() {
		QSignalBlocker b1(restoreAct_), b2(captureAct_);
		restoreAct_->setChecked(c_->doc().restoreOutputs);
		captureAct_->setChecked(c_->doc().excludeFromCapture);
	});
	connect(restoreAct_, &QAction::toggled, this, [this](bool on) { c_->setRestoreOutputs(on); });
	connect(captureAct_, &QAction::toggled, this, [this](bool on) { c_->setExcludeFromCapture(on); });
	optBtn->setMenu(optMenu);
	tb->addWidget(optBtn);

	tb->addAction(QStringLiteral("▶ Start All"), this, [this]() { c_->startAll(); })
		->setToolTip(QStringLiteral("Start fullscreen on every display that is connected and identified"));
	tb->addAction(QStringLiteral("■ Stop All"), this, [this]() { c_->stopAll(); })
		->setToolTip(QStringLiteral("Stop every output (fullscreen and windows)"));
	root->addWidget(tb);

	/* --- body --- */
	auto *body = new QSplitter(Qt::Horizontal, this);
	body->setChildrenCollapsible(false);

	auto *side = new QSplitter(Qt::Vertical, body);
	side->setChildrenCollapsible(false);

	auto *treeWrap = new QWidget(side);
	auto *tl = new QVBoxLayout(treeWrap);
	tl->setContentsMargins(8, 8, 2, 0);
	auto *treeTitle = new QLabel(QStringLiteral("DISPLAYS & SLICES"), treeWrap);
	treeTitle->setObjectName("PanelTitle");
	tl->addWidget(treeTitle);
	sideStack_ = new QStackedWidget(treeWrap);
	sideStack_->addWidget(buildEmptyState());
	tree_ = new SliceTree(sideStack_);
	sideStack_->addWidget(tree_);
	tl->addWidget(sideStack_);
	auto *hint = new QLabel(QStringLiteral("The top slice of a display is drawn on top. Drag slices to reorder "
					       "them or move them to another display."),
				treeWrap);
	hint->setObjectName("SectionHint");
	hint->setWordWrap(true);
	tl->addWidget(hint);

	auto *inspScroll = new QScrollArea(side);
	inspScroll->setWidgetResizable(true);
	inspScroll->setFrameShape(QFrame::NoFrame);
	inspScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	inspScroll->setWidget(buildInspector());
	side->addWidget(treeWrap);
	side->addWidget(inspScroll);
	side->setStretchFactor(0, 2);
	side->setStretchFactor(1, 3);
	side->setMinimumWidth(420);

	auto *previews = new QSplitter(Qt::Horizontal, body);
	previews->setChildrenCollapsible(false);
	input_ = new PreviewView(PreviewView::Kind::Input);
	output_ = new PreviewView(PreviewView::Kind::Output);
	previews->addWidget(buildPreviewPanel(input_, inTitle_, inSub_, inZoom_));
	previews->addWidget(buildPreviewPanel(output_, outTitle_, outSub_, outZoom_));
	inTitle_->setText(QStringLiteral("INPUT SELECTION"));
	inTitle_->setToolTip(QStringLiteral("The live OBS Program. Drag to draw a slice; move or resize a slice to "
					    "change which pixels it grabs (its placement stays put)."));
	outTitle_->setText(QStringLiteral("OUTPUT PLACEMENT"));
	outTitle_->setToolTip(QStringLiteral("Exactly what the selected display receives. Move or resize a slice to "
					     "place it (its crop stays put)."));
	previews->setSizes({600, 600});

	body->addWidget(side);
	body->addWidget(previews);
	body->setStretchFactor(0, 0);
	body->setStretchFactor(1, 1);
	body->setSizes({440, 1060});
	root->addWidget(body, 1);

	/* --- status bar --- */
	auto *sb = new QWidget(this);
	sb->setObjectName("StatusBar");
	auto *sl = new QHBoxLayout(sb);
	sl->setContentsMargins(0, 0, 0, 0);
	statusLeft_ = new QLabel(sb);
	statusLeft_->setObjectName("Status");
	statusRight_ = new QLabel(sb);
	statusRight_->setObjectName("Status");
	sl->addWidget(statusLeft_, 1);
	sl->addWidget(statusRight_);
	root->addWidget(sb);

	/* --- wiring --- */
	connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MappingEditor::onTreeSelection);
	connect(tree_, &QTreeWidget::itemChanged, this, &MappingEditor::onTreeItemChanged);
	connect(tree_, &QTreeWidget::customContextMenuRequested, this, &MappingEditor::showTreeMenu);

	connect(snapAct_, &QAction::toggled, this, [this](bool on) {
		input_->setSnapping(on);
		output_->setSnapping(on);
	});
	connect(lockAct_, &QAction::toggled, this, [this](bool on) {
		input_->setAspectLock(on);
		output_->setAspectLock(on);
		QSignalBlocker b(lockCheck_);
		lockCheck_->setChecked(on);
	});
	connect(lockCheck_, &QCheckBox::toggled, lockAct_, &QAction::setChecked);

	for (PreviewView *v : {input_, output_}) {
		connect(v, &PreviewView::hoverChanged, this, [this, v](QPointF p, bool inside) {
			if (!inside) {
				updateStatusBar();
				return;
			}
			statusLeft_->setText(QStringLiteral("%1  x %2  y %3")
						     .arg(v == input_ ? "PROGRAM" : "DISPLAY")
						     .arg((int)std::floor(p.x()))
						     .arg((int)std::floor(p.y())));
		});
		connect(v, &PreviewView::needDisplay, this, [this]() { addDisplayBtn_->showMenu(); });
	}
	connect(input_, &PreviewView::zoomChanged, this,
		[this](double z) { inZoom_->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });
	connect(output_, &PreviewView::zoomChanged, this,
		[this](double z) { outZoom_->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });
}

QWidget *MappingEditor::buildEmptyState()
{
	auto *w = new QFrame(this);
	w->setObjectName("EmptyState");
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(18, 18, 18, 18);
	v->setSpacing(10);
	auto *title = new QLabel(QStringLiteral("No displays yet"), w);
	title->setObjectName("EmptyTitle");
	auto *text = new QLabel(QStringLiteral("1.  Add the display (HDMI output) to send video to.\n"
					       "2.  Drag on the Program image in Input Selection to draw a slice.\n"
					       "3.  Position it in Output Placement, then Start Fullscreen."),
				w);
	text->setWordWrap(true);
	text->setObjectName("SectionHint");
	text->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
	text->setMinimumHeight(text->fontMetrics().lineSpacing() * 4);
	auto *add = new QPushButton(QStringLiteral("＋ Add Display"), w);
	add->setObjectName("Primary");
	auto *menu = new QMenu(add);
	connect(menu, &QMenu::aboutToShow, this, [this, menu]() { populateAddDisplayMenu(menu); });
	add->setMenu(menu);
	auto *virt = new QPushButton(QStringLiteral("Add Virtual Display for Testing…"), w);
	connect(virt, &QPushButton::clicked, this, &MappingEditor::addVirtualDisplay);
	v->addWidget(title);
	v->addWidget(text);
	v->addSpacing(4);
	v->addWidget(add);
	v->addWidget(virt);
	v->addStretch(1);
	return w;
}

QWidget *MappingEditor::buildPreviewPanel(PreviewView *view, QLabel *&title, QLabel *&subtitle, QLabel *&zoomLabel)
{
	auto *panel = new QFrame(this);
	panel->setObjectName("Panel");
	auto *v = new QVBoxLayout(panel);
	v->setContentsMargins(10, 8, 10, 10);
	v->setSpacing(6);

	/* Row 1: title + view controls. Row 2: context (size, target display). */
	auto *head = new QHBoxLayout();
	title = new QLabel(panel);
	title->setObjectName("PanelTitle");
	subtitle = new QLabel(panel);
	subtitle->setObjectName("PanelSub");
	subtitle->setTextFormat(Qt::RichText);
	zoomLabel = new QLabel(QStringLiteral("100%"), panel);
	zoomLabel->setObjectName("Zoom");
	auto mk = [&](const QString &t, const QString &tip, auto fn) {
		auto *b = new QToolButton(panel);
		b->setText(t);
		b->setToolTip(tip);
		b->setAutoRaise(true);
		connect(b, &QToolButton::clicked, view, fn);
		return b;
	};
	head->addWidget(title);
	head->addStretch(1);
	head->addWidget(mk(QStringLiteral("−"), QStringLiteral("Zoom out (−)"), [view]() { view->zoomBy(0.8); }));
	head->addWidget(zoomLabel);
	head->addWidget(mk(QStringLiteral("+"), QStringLiteral("Zoom in (+)"), [view]() { view->zoomBy(1.25); }));
	head->addWidget(mk(QStringLiteral("100%"), QStringLiteral("Actual pixels (1)"), [view]() { view->zoomActual(); }));
	head->addWidget(mk(QStringLiteral("Fit"), QStringLiteral("Fit to view (F or double-click)"),
			   [view]() { view->fitToView(); }));
	v->addLayout(head);

	auto *ctx = new QHBoxLayout();
	ctx->setSpacing(8);
	ctx->addWidget(subtitle);
	if (view == input_) {
		targetChip_ = new QLabel(panel);
		targetChip_->setObjectName("Chip");
		targetChip_->setToolTip(QStringLiteral("Slices you draw here are added to this display. Select another "
						       "display in the sidebar to draw for it."));
		ctx->addWidget(targetChip_);
	}
	ctx->addStretch(1);
	v->addLayout(ctx);
	v->addWidget(view, 1);
	return panel;
}

static QSpinBox *makeSpin(QWidget *parent, int min)
{
	auto *s = new QSpinBox(parent);
	s->setRange(min, 65535);
	s->setKeyboardTracking(false);
	s->setButtonSymbols(QAbstractSpinBox::NoButtons);
	s->setAlignment(Qt::AlignRight);
	s->setMinimumWidth(46);
	s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	return s;
}

QWidget *MappingEditor::buildInspector()
{
	auto *w = new QWidget(this);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(8, 0, 2, 8);
	v->setSpacing(4);

	noSelection_ = new QLabel(QStringLiteral("Select a display or slice in the list above."), w);
	noSelection_->setObjectName("SectionHint");
	noSelection_->setWordWrap(true);
	v->addWidget(noSelection_);

	/* ---- slice (first: the fields you edit most) ---- */
	sliceBox_ = new QGroupBox(QStringLiteral("SLICE"), w);
	auto *sv = new QVBoxLayout(sliceBox_);
	sv->setSpacing(6);

	auto *nameRow = new QHBoxLayout();
	sliceSwatch_ = new QLabel(sliceBox_);
	sliceSwatch_->setFixedSize(14, 14);
	sliceName_ = new QLineEdit(sliceBox_);
	sliceName_->setPlaceholderText(QStringLiteral("Slice name"));
	sliceEnabled_ = new QCheckBox(QStringLiteral("Visible"), sliceBox_);
	nameRow->addWidget(sliceSwatch_);
	nameRow->addWidget(sliceName_, 1);
	nameRow->addWidget(sliceEnabled_);
	sv->addLayout(nameRow);

	auto *grid = new QGridLayout();
	grid->setHorizontalSpacing(4);
	grid->setVerticalSpacing(4);
	const char *heads[] = {"X", "Y", "W", "H"};
	for (int i = 0; i < 4; i++) {
		auto *h = new QLabel(QString::fromLatin1(heads[i]), sliceBox_);
		h->setAlignment(Qt::AlignCenter);
		h->setObjectName("SectionHint");
		grid->addWidget(h, 0, i + 1);
	}
	auto *srcL = new QLabel(QStringLiteral("Source"), sliceBox_);
	srcL->setToolTip(QStringLiteral("Which part of Program the slice grabs, in Program pixels"));
	auto *dstL = new QLabel(QStringLiteral("Output"), sliceBox_);
	dstL->setToolTip(QStringLiteral("Where it appears on the display, in physical display pixels"));
	grid->addWidget(srcL, 1, 0);
	grid->addWidget(dstL, 2, 0);
	for (int i = 0; i < 4; i++) {
		src_[i] = makeSpin(sliceBox_, i < 2 ? -65535 : 1);
		dst_[i] = makeSpin(sliceBox_, i < 2 ? -65535 : 1);
		grid->addWidget(src_[i], 1, i + 1);
		grid->addWidget(dst_[i], 2, i + 1);
	}
	sv->addLayout(grid);
	scaleLabel_ = new QLabel(sliceBox_);
	scaleLabel_->setObjectName("SectionHint");
	scaleLabel_->setTextFormat(Qt::RichText);
	sv->addWidget(scaleLabel_);

	auto *sf = new QFormLayout();
	sf->setLabelAlignment(Qt::AlignRight);
	sf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	sliceDisplay_ = new QComboBox(sliceBox_);
	sliceDisplay_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	sliceDisplay_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	sliceDisplay_->setMinimumContentsLength(10);
	sliceDisplay_->setToolTip(QStringLiteral("Move this slice to another display"));
	sf->addRow(QStringLiteral("On display"), sliceDisplay_);
	sv->addLayout(sf);

	lockCheck_ = new QCheckBox(QStringLiteral("Lock aspect ratio"), sliceBox_);
	sv->addWidget(lockCheck_);
	auto *sb = new QGridLayout();
	auto *fitBtn = new QPushButton(QStringLiteral("Fit to Display"), sliceBox_);
	auto *oneBtn = new QPushButton(QStringLiteral("1:1 Pixels"), sliceBox_);
	auto *upBtn = new QPushButton(QStringLiteral("Bring Forward"), sliceBox_);
	auto *downBtn = new QPushButton(QStringLiteral("Send Backward"), sliceBox_);
	sb->addWidget(fitBtn, 0, 0);
	sb->addWidget(oneBtn, 0, 1);
	sb->addWidget(upBtn, 1, 0);
	sb->addWidget(downBtn, 1, 1);
	sv->addLayout(sb);
	v->addWidget(sliceBox_);

	/* ---- display ---- */
	displayBox_ = new QGroupBox(QStringLiteral("DISPLAY"), w);
	auto *df = new QFormLayout(displayBox_);
	df->setLabelAlignment(Qt::AlignRight);
	df->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	displayName_ = new QLineEdit(displayBox_);
	screenCombo_ = new QComboBox(displayBox_);
	screenCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	screenCombo_->setMinimumContentsLength(14);
	screenCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	screenCombo_->setToolTip(QStringLiteral("The physical display this output goes to"));
	displayStatus_ = new QLabel(displayBox_);
	displayStatus_->setWordWrap(true);
	displayStatus_->setTextFormat(Qt::RichText);
	virtRow_ = new QWidget(displayBox_);
	auto *virtRow = new QHBoxLayout(virtRow_);
	virtRow->setContentsMargins(0, 0, 0, 0);
	virtW_ = makeSpin(virtRow_, 16);
	virtH_ = makeSpin(virtRow_, 16);
	virtRow->addWidget(virtW_);
	virtRow->addWidget(new QLabel(QStringLiteral("×"), virtRow_));
	virtRow->addWidget(virtH_);
	resLabel_ = new QLabel(displayBox_);
	fullscreenBtn_ = new QPushButton(QStringLiteral("▶ Start Fullscreen"), displayBox_);
	fullscreenBtn_->setObjectName("Primary");
	windowBtn_ = new QPushButton(QStringLiteral("Open Window"), displayBox_);
	windowBtn_->setToolTip(QStringLiteral("Preview this output in a normal window"));
	snapshotBtn_ = new QPushButton(QStringLiteral("Snapshot…"), displayBox_);
	snapshotBtn_->setToolTip(QStringLiteral("Save a PNG of exactly what this display receives"));
	removeDisplayBtn_ = new QPushButton(QStringLiteral("Remove Display"), displayBox_);
	removeDisplayBtn_->setObjectName("Danger");
	auto *btnGrid = new QGridLayout();
	btnGrid->addWidget(fullscreenBtn_, 0, 0, 1, 2);
	btnGrid->addWidget(windowBtn_, 1, 0);
	btnGrid->addWidget(snapshotBtn_, 1, 1);
	btnGrid->addWidget(removeDisplayBtn_, 2, 0, 1, 2);
	df->addRow(QStringLiteral("Name"), displayName_);
	df->addRow(QStringLiteral("Output to"), screenCombo_);
	df->addRow(QStringLiteral("Status"), displayStatus_);
	df->addRow(QStringLiteral("Pixels"), resLabel_);
	df->addRow(QStringLiteral("Size"), virtRow_);
	df->addRow(btnGrid);
	v->addWidget(displayBox_);
	v->addStretch(1);

	/* ---- wiring ---- */
	connect(displayName_, &QLineEdit::editingFinished, this,
		[this]() { c_->renameDisplay(c_->selectedDisplay(), displayName_->text()); });
	connect(screenCombo_, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
		QVariant d = screenCombo_->itemData(idx);
		if (!d.isValid())
			return;
		c_->assignScreen(c_->selectedDisplay(), d.toInt());
	});
	auto pushVirt = [this](int) {
		if (!updating_)
			c_->setVirtualSize(c_->selectedDisplay(), virtW_->value(), virtH_->value());
	};
	connect(virtW_, qOverload<int>(&QSpinBox::valueChanged), this, pushVirt);
	connect(virtH_, qOverload<int>(&QSpinBox::valueChanged), this, pushVirt);
	connect(fullscreenBtn_, &QPushButton::clicked, this, &MappingEditor::startFullscreenSelected);
	connect(windowBtn_, &QPushButton::clicked, this, &MappingEditor::toggleWindowSelected);
	connect(snapshotBtn_, &QPushButton::clicked, this, &MappingEditor::snapshotSelected);
	connect(removeDisplayBtn_, &QPushButton::clicked, this, [this]() {
		const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
		if (!d)
			return;
		if (QMessageBox::question(this, QStringLiteral("Remove Display"),
					  QStringLiteral("Remove “%1” and its %2 slice(s)?\n\nYou can undo this.")
						  .arg(d->name)
						  .arg((qulonglong)d->slices.size())) == QMessageBox::Yes)
			c_->removeDisplay(d->id);
	});

	connect(sliceName_, &QLineEdit::editingFinished, this,
		[this]() { c_->renameSlice(c_->selectedSlice(), sliceName_->text()); });
	connect(sliceEnabled_, &QCheckBox::toggled, this, [this](bool on) {
		if (!updating_)
			c_->setSliceEnabled(c_->selectedSlice(), on);
	});
	connect(sliceDisplay_, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
		QString did = sliceDisplay_->itemData(idx).toString();
		c_->moveSlice(c_->selectedSlice(), did, 0);
	});
	for (int i = 0; i < 4; i++) {
		connect(src_[i], qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
			if (!updating_)
				pushSrcFromFields();
		});
		connect(dst_[i], qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int) {
			if (!updating_)
				pushDstFromFields(i == 2, i == 3);
		});
	}
	connect(fitBtn, &QPushButton::clicked, this, [this]() { c_->fitSliceToDisplay(c_->selectedSlice()); });
	connect(oneBtn, &QPushButton::clicked, this, [this]() { c_->sliceOneToOne(c_->selectedSlice()); });
	connect(upBtn, &QPushButton::clicked, this, [this]() { moveSelectedSlice(-1); });
	connect(downBtn, &QPushButton::clicked, this, [this]() { moveSelectedSlice(+1); });
	return w;
}

/* ---------------------------------------------------------------------- */
/* Sync from model                                                         */

void MappingEditor::rebuildTree()
{
	updating_ = true;
	sideStack_->setCurrentIndex(c_->doc().displays.empty() ? 0 : 1);
	QSignalBlocker b(tree_);
	tree_->clear();
	for (const DisplayMapping &d : c_->doc().displays) {
		DisplayStatus st = c_->status(d.id);
		OutputMode m = c_->outputMode(d.id);
		auto *di = new QTreeWidgetItem(tree_);
		di->setText(0, QStringLiteral("%1   %2×%3").arg(d.name).arg(d.width).arg(d.height));
		di->setIcon(0, dotIcon(m == OutputMode::Fullscreen ? QColor(theme::kLive) : statusColor(st, d.kind)));
		QColor badgeColor;
		di->setText(1, statusBadge(st, d.kind, m, badgeColor));
		di->setForeground(1, badgeColor);
		QFont bf = di->font(1);
		bf.setBold(true);
		di->setFont(1, bf);
		di->setToolTip(0, st.text);
		di->setToolTip(1, st.text);
		di->setData(0, RoleKind, KindDisplay);
		di->setData(0, RoleId, d.id);
		di->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
		QFont f = di->font(0);
		f.setBold(true);
		di->setFont(0, f);
		for (const Slice &s : d.slices) {
			auto *si = new QTreeWidgetItem(di);
			si->setText(0, s.name);
			si->setIcon(0, dotIcon(s.color, true));
			const bool oneToOne = s.src.w == s.dst.w && s.src.h == s.dst.h;
			si->setText(1, !s.enabled ? QStringLiteral("hidden")
						   : oneToOne ? QStringLiteral("1:1")
							      : QStringLiteral("scaled"));
			si->setForeground(1, QColor(theme::kMuted));
			si->setToolTip(0, QStringLiteral("Source %1,%2 %3×%4  →  Output %5,%6 %7×%8")
						  .arg(s.src.x)
						  .arg(s.src.y)
						  .arg(s.src.w)
						  .arg(s.src.h)
						  .arg(s.dst.x)
						  .arg(s.dst.y)
						  .arg(s.dst.w)
						  .arg(s.dst.h));
			si->setData(0, RoleKind, KindSlice);
			si->setData(0, RoleId, s.id);
			si->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
			if (!s.enabled) {
				QFont sf = si->font(0);
				sf.setItalic(true);
				si->setFont(0, sf);
				si->setForeground(0, QColor(theme::kMuted));
			}
		}
		di->setExpanded(true);
	}
	updating_ = false;
	syncTreeSelection();
}

void MappingEditor::syncTreeSelection()
{
	QSignalBlocker b(tree_);
	const QString want = c_->selectedSlice().isEmpty() ? c_->selectedDisplay() : c_->selectedSlice();
	QTreeWidgetItemIterator it(tree_);
	tree_->clearSelection();
	while (*it) {
		if ((*it)->data(0, RoleId).toString() == want && !want.isEmpty()) {
			(*it)->setSelected(true);
			tree_->setCurrentItem(*it);
			tree_->scrollToItem(*it);
			break;
		}
		++it;
	}
}

void MappingEditor::rebuildScreenCombo()
{
	QSignalBlocker b(screenCombo_);
	screenCombo_->clear();
	const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
	if (!d)
		return;
	DisplayStatus st = c_->status(d->id);
	int current = -1;
	if (d->kind == DisplayKind::Physical && !isConnected(st.match)) {
		QString what = st.match == MatchStatus::Ambiguous ? QStringLiteral("needs confirming")
								  : QStringLiteral("not connected");
		screenCombo_->addItem(dotIcon(statusColor(st, d->kind)),
				      QStringLiteral("%1 (%2)").arg(QString::fromStdString(d->identity.name), what));
		current = 0;
	}
	const auto &screens = c_->screens();
	for (int i = 0; i < (int)screens.size(); i++) {
		const ScreenInfo &s = screens[(size_t)i];
		QString label = QStringLiteral("%1 — %2×%3").arg(s.name).arg(s.pixelW).arg(s.pixelH);
		if (s.builtin)
			label += QStringLiteral(" (built-in)");
		if (s.primary)
			label += QStringLiteral(" (main)");
		bool candidate = std::find(st.candidates.begin(), st.candidates.end(), i) != st.candidates.end();
		if (candidate)
			label += QStringLiteral("  ← possible match");
		screenCombo_->addItem(label, i);
		if (isConnected(st.match) && st.screen == i && d->kind == DisplayKind::Physical)
			current = screenCombo_->count() - 1;
	}
	screenCombo_->addItem(QStringLiteral("Virtual (window only)"), -1);
	if (d->kind == DisplayKind::Virtual)
		current = screenCombo_->count() - 1;
	screenCombo_->setCurrentIndex(std::max(0, current));

	QSignalBlocker b2(sliceDisplay_);
	sliceDisplay_->clear();
	for (const DisplayMapping &dm : c_->doc().displays)
		sliceDisplay_->addItem(dm.name, dm.id);
	sliceDisplay_->setCurrentIndex(c_->doc().displayIndex(d->id));
}

void MappingEditor::updateInspector()
{
	updating_ = true;
	const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
	const Slice *s = c_->doc().slice(c_->selectedSlice());

	noSelection_->setVisible(!d && !c_->doc().displays.empty());
	displayBox_->setVisible(d != nullptr);
	sliceBox_->setVisible(s != nullptr);

	if (d) {
		DisplayStatus st = c_->status(d->id);
		if (!displayName_->hasFocus())
			displayName_->setText(d->name);
		QString text = st.text.toHtmlEscaped();
		if (isConnected(st.match) && !st.exactPixels)
			text += QStringLiteral("<br><span style='color:%1'>The OS is scaling this display, so output "
					       "is resampled, not 1:1. Use the display's native resolution for "
					       "pixel-exact output.</span>")
					.arg(theme::kWarn);
		if (st.match == MatchStatus::Ambiguous)
			text += QStringLiteral("<br><span style='color:%1'>A display like this is connected, but it "
					       "can't be confirmed as the same one. Pick it under “Output to” to "
					       "confirm. Fullscreen stays off until then.</span>")
					.arg(theme::kWarn);
		if (st.match == MatchStatus::Disconnected)
			text += QStringLiteral("<br><span style='color:%1'>Nothing is shown on another screen. The "
					       "output resumes when this display is reconnected.</span>")
					.arg(theme::kMuted);
		displayStatus_->setText(QStringLiteral("<span style='color:%1'>●</span> %2")
						.arg(statusColor(st, d->kind).name(), text));

		const bool isVirtual = d->kind == DisplayKind::Virtual;
		auto *form = qobject_cast<QFormLayout *>(displayBox_->layout());
		if (form) {
			form->setRowVisible(virtRow_, isVirtual);
			form->setRowVisible(resLabel_, !isVirtual);
		}
		virtW_->setValue(d->width);
		virtH_->setValue(d->height);
		resLabel_->setText(QStringLiteral("%1 × %2").arg(d->width).arg(d->height));

		OutputMode m = c_->outputMode(d->id);
		if (m == OutputMode::Fullscreen) {
			fullscreenBtn_->setText(QStringLiteral("■ Stop Fullscreen"));
			fullscreenBtn_->setObjectName("Live");
			fullscreenBtn_->setEnabled(true);
			fullscreenBtn_->setToolTip(QString());
		} else {
			fullscreenBtn_->setText(QStringLiteral("▶ Start Fullscreen"));
			fullscreenBtn_->setObjectName("Primary");
			const bool ok = !isVirtual && isConnected(st.match) && st.canFullscreen;
			fullscreenBtn_->setEnabled(ok);
			if (isVirtual)
				fullscreenBtn_->setToolTip(QStringLiteral("Virtual displays only open as a window. Pick a "
									  "physical display under “Output to” to go "
									  "fullscreen."));
			else
				fullscreenBtn_->setToolTip(ok ? QString() : QStringLiteral("Unavailable: %1").arg(st.text));
		}
		fullscreenBtn_->style()->unpolish(fullscreenBtn_);
		fullscreenBtn_->style()->polish(fullscreenBtn_);
		windowBtn_->setText(m == OutputMode::Window ? QStringLiteral("Close Window") : QStringLiteral("Open Window"));
	}

	if (s) {
		if (!sliceName_->hasFocus())
			sliceName_->setText(s->name);
		sliceSwatch_->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;").arg(s->color.name()));
		sliceEnabled_->setChecked(s->enabled);
		const int sv[4] = {s->src.x, s->src.y, s->src.w, s->src.h};
		const int dv[4] = {s->dst.x, s->dst.y, s->dst.w, s->dst.h};
		for (int i = 0; i < 4; i++) {
			src_[i]->setValue(sv[i]);
			dst_[i]->setValue(dv[i]);
		}
		const bool oneToOne = s->src.w == s->dst.w && s->src.h == s->dst.h;
		if (oneToOne)
			scaleLabel_->setText(
				QStringLiteral("<span style='color:%1'>●</span> 1:1, pixel for pixel").arg(theme::kOk));
		else
			scaleLabel_->setText(QStringLiteral("Scaled %1% × %2%. Use 1:1 Pixels for exact output.")
						     .arg(100.0 * s->dst.w / std::max(1, s->src.w), 0, 'f', 1)
						     .arg(100.0 * s->dst.h / std::max(1, s->src.h), 0, 'f', 1));
	}
	updating_ = false;
}

void MappingEditor::updateHeaders()
{
	int pw = 0, ph = 0;
	c_->programSize(pw, ph);
	inSub_->setText(QStringLiteral("Program %1 × %2").arg(pw).arg(ph));
	const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
	if (targetChip_) {
		targetChip_->setVisible(d != nullptr);
		if (d)
			targetChip_->setText(QStringLiteral("new slices → %1").arg(d->name));
	}
	if (d) {
		OutputMode m = c_->outputMode(d->id);
		QString live;
		if (m == OutputMode::Fullscreen)
			live = QStringLiteral("  <span style='color:%1'>● LIVE</span>").arg(theme::kLive);
		else if (m == OutputMode::Window)
			live = QStringLiteral("  <span style='color:%1'>◻ window</span>").arg(theme::kAccent);
		outSub_->setText(
			QStringLiteral("%1 · %2 × %3 px%4").arg(d->name.toHtmlEscaped()).arg(d->width).arg(d->height).arg(live));
	} else {
		outSub_->setText(QStringLiteral("no display selected"));
	}
}

void MappingEditor::updateActions()
{
	const bool hasSlice = !c_->selectedSlice().isEmpty();
	const bool hasDisplay = !c_->selectedDisplay().isEmpty();
	addSliceAct_->setEnabled(hasDisplay);
	dupAct_->setEnabled(hasSlice);
	delAct_->setEnabled(hasSlice);
	fitAct_->setEnabled(hasSlice);
	oneAct_->setEnabled(hasSlice);
	undoAct_->setEnabled(c_->canUndo());
	redoAct_->setEnabled(c_->canRedo());
}

void MappingEditor::updateStatusBar()
{
	int pw = 0, ph = 0;
	c_->programSize(pw, ph);
	statusLeft_->setText(QStringLiteral("Drag on Input Selection to draw · arrows nudge (Shift ×10) · Space-drag "
					    "pans · %1+scroll zooms · hold %2 to bypass snapping")
				     .arg(modKeyName(), altKeyName()));
	statusRight_->setText(
		QStringLiteral("Program %1×%2 · %3 output(s) running").arg(pw).arg(ph).arg(c_->activeOutputCount()));
}

/* ---------------------------------------------------------------------- */
/* Actions                                                                 */

void MappingEditor::onTreeSelection()
{
	if (updating_)
		return;
	auto sel = tree_->selectedItems();
	if (sel.isEmpty())
		return;
	QTreeWidgetItem *it = sel.first();
	QString id = it->data(0, RoleId).toString();
	if (it->data(0, RoleKind).toInt() == KindDisplay)
		c_->select(id, QString());
	else
		c_->selectSlice(id);
}

void MappingEditor::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
	if (updating_ || column != 0 || item->data(0, RoleKind).toInt() != KindSlice)
		return;
	QString id = item->data(0, RoleId).toString();
	QString name = item->text(0);
	QMetaObject::invokeMethod(this, [this, id, name]() { c_->renameSlice(id, name); }, Qt::QueuedConnection);
}

void MappingEditor::showTreeMenu(const QPoint &pos)
{
	QTreeWidgetItem *it = tree_->itemAt(pos);
	QMenu menu(this);
	if (it && it->data(0, RoleKind).toInt() == KindSlice) {
		QString id = it->data(0, RoleId).toString();
		c_->selectSlice(id);
		menu.addAction(QStringLiteral("Rename"), this, [this, it]() { tree_->editItem(it, 0); });
		menu.addAction(QStringLiteral("Duplicate"), this, [this, id]() { c_->duplicateSlice(id); });
		menu.addAction(QStringLiteral("Bring Forward"), this, [this]() { moveSelectedSlice(-1); });
		menu.addAction(QStringLiteral("Send Backward"), this, [this]() { moveSelectedSlice(+1); });
		QMenu *to = menu.addMenu(QStringLiteral("Move to Display"));
		const DisplayMapping *owner = c_->doc().displayForSlice(id);
		for (const DisplayMapping &d : c_->doc().displays) {
			QAction *a = to->addAction(d.name, this, [this, id, did = d.id]() { c_->moveSlice(id, did, 0); });
			a->setEnabled(!owner || owner->id != d.id);
		}
		menu.addSeparator();
		menu.addAction(QStringLiteral("Fit to Display"), this, [this, id]() { c_->fitSliceToDisplay(id); });
		menu.addAction(QStringLiteral("1:1 Pixels"), this, [this, id]() { c_->sliceOneToOne(id); });
		menu.addSeparator();
		menu.addAction(QStringLiteral("Delete"), this, [this, id]() { c_->deleteSlice(id); });
	} else if (it) {
		QString id = it->data(0, RoleId).toString();
		c_->select(id, QString());
		menu.addAction(QStringLiteral("Add Slice"), this, &MappingEditor::addSliceToSelected);
		QAction *fs = menu.addAction(c_->outputMode(id) == OutputMode::Fullscreen ? QStringLiteral("Stop Fullscreen")
											   : QStringLiteral("Start Fullscreen"),
					     this, &MappingEditor::startFullscreenSelected);
		fs->setEnabled(fullscreenBtn_->isEnabled());
		menu.addAction(c_->outputMode(id) == OutputMode::Window ? QStringLiteral("Close Window")
									: QStringLiteral("Open Window"),
			       this, &MappingEditor::toggleWindowSelected);
		menu.addSeparator();
		menu.addAction(QStringLiteral("Move Up"), this,
			       [this, id]() { c_->moveDisplay(id, c_->doc().displayIndex(id) - 1); });
		menu.addAction(QStringLiteral("Move Down"), this,
			       [this, id]() { c_->moveDisplay(id, c_->doc().displayIndex(id) + 1); });
		menu.addSeparator();
		menu.addAction(QStringLiteral("Remove Display"), this, [this, id]() { c_->removeDisplay(id); });
	} else {
		QMenu *add = menu.addMenu(QStringLiteral("Add Display"));
		populateAddDisplayMenu(add);
	}
	menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void MappingEditor::populateAddDisplayMenu(QMenu *m)
{
	m->clear();
	c_->rescanScreens();
	const auto &screens = c_->screens();
	QAction *head = m->addAction(QStringLiteral("Connected displays"));
	head->setEnabled(false);
	for (int i = 0; i < (int)screens.size(); i++) {
		const ScreenInfo &s = screens[(size_t)i];
		QString label = QStringLiteral("%1 — %2×%3").arg(s.name).arg(s.pixelW).arg(s.pixelH);
		if (s.builtin)
			label += QStringLiteral(" (built-in)");
		if (s.primary)
			label += QStringLiteral(" (main)");
		bool used = false;
		for (const DisplayMapping &d : c_->doc().displays)
			if (d.kind == DisplayKind::Physical && d.identity.uuid == s.identity.uuid)
				used = true;
		if (used)
			label += QStringLiteral("  ✓ added");
		m->addAction(label, this, [this, i]() { c_->addPhysicalDisplay(i); });
	}
	m->addSeparator();
	m->addAction(QStringLiteral("Virtual Display for Testing (window only)…"), this, &MappingEditor::addVirtualDisplay);
}

void MappingEditor::addVirtualDisplay()
{
	bool ok = false;
	QString res = QInputDialog::getText(this, QStringLiteral("Virtual Display"),
					    QStringLiteral("A virtual display has a fixed resolution and opens as a "
							   "window.\n\nResolution (width × height):"),
					    QLineEdit::Normal, QStringLiteral("1920x1080"), &ok);
	if (!ok)
		return;
	QStringList parts = res.split(QRegularExpression("[x×X* ,]+"), Qt::SkipEmptyParts);
	int w = parts.size() > 0 ? parts[0].toInt() : 1920;
	int h = parts.size() > 1 ? parts[1].toInt() : 1080;
	if (w < 16 || h < 16 || w > 16384 || h > 16384) {
		QMessageBox::warning(this, QStringLiteral("Virtual Display"),
				     QStringLiteral("Enter a resolution like 1920x1080."));
		return;
	}
	c_->addVirtualDisplay(QString(), w, h);
}

void MappingEditor::addSliceToSelected()
{
	const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
	if (!d)
		return;
	int pw = 0, ph = 0;
	c_->programSize(pw, ph);
	IRect src{0, 0, std::min(pw, d->width), std::min(ph, d->height)};
	if (src.empty())
		src = {0, 0, d->width, d->height};
	c_->createSlice(d->id, src);
}

void MappingEditor::startFullscreenSelected()
{
	const QString id = c_->selectedDisplay();
	if (id.isEmpty())
		return;
	if (c_->outputMode(id) == OutputMode::Fullscreen) {
		c_->stopOutput(id);
		return;
	}
	DisplayStatus st = c_->status(id);
	if (isConnected(st.match) && st.screen >= 0 && st.screen < (int)c_->screens().size()) {
		QScreen *target = c_->screens()[(size_t)st.screen].screen;
		QScreen *mine = windowHandle() ? windowHandle()->screen() : nullptr;
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (target && (target == mine || (main && main->screen() == target))) {
			auto r = QMessageBox::question(
				this, QStringLiteral("Cover this screen?"),
				QStringLiteral("This display is showing OBS. The output will cover it.\n\n"
					       "To stop it: click the output and press Esc, or use Tools ▸ Output "
					       "Mapping: Stop All Outputs, or the “Stop all outputs” hotkey."));
			if (r != QMessageBox::Yes)
				return;
		}
	}
	QString err;
	if (!c_->startFullscreen(id, &err))
		QMessageBox::warning(this, QStringLiteral("Can't Start Fullscreen"), err);
}

void MappingEditor::toggleWindowSelected()
{
	const QString id = c_->selectedDisplay();
	if (id.isEmpty())
		return;
	if (c_->outputMode(id) == OutputMode::Window)
		c_->stopOutput(id);
	else
		c_->openWindow(id);
}

void MappingEditor::snapshotSelected()
{
	const DisplayMapping *d = c_->doc().display(c_->selectedDisplay());
	if (!d)
		return;
	QString err;
	QImage img = c_->snapshot(d->id, &err);
	if (img.isNull()) {
		QMessageBox::warning(this, QStringLiteral("Snapshot"), err);
		return;
	}
	QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	QString safe = QString(d->name).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
	QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Output Snapshot"),
						    dir + "/" + safe + ".png", QStringLiteral("PNG (*.png)"));
	if (!path.isEmpty() && !img.save(path, "PNG"))
		QMessageBox::warning(this, QStringLiteral("Snapshot"), QStringLiteral("Could not save %1").arg(path));
}

void MappingEditor::moveSelectedSlice(int delta)
{
	const QString id = c_->selectedSlice();
	const DisplayMapping *d = c_->doc().displayForSlice(id);
	if (!d)
		return;
	c_->moveSlice(id, d->id, d->sliceIndex(id) + delta);
}

void MappingEditor::pushSrcFromFields()
{
	const Slice *s = c_->doc().slice(c_->selectedSlice());
	if (!s)
		return;
	IRect r{src_[0]->value(), src_[1]->value(), src_[2]->value(), src_[3]->value()};
	if (lockAct_->isChecked() && (r.w != s->src.w || r.h != s->src.h))
		r = applySizeEdit(s->src, r.w, r.h, r.w != s->src.w, true);
	c_->setSliceSrc(s->id, r);
}

void MappingEditor::pushDstFromFields(bool widthChanged, bool heightChanged)
{
	const Slice *s = c_->doc().slice(c_->selectedSlice());
	if (!s)
		return;
	IRect r{dst_[0]->value(), dst_[1]->value(), dst_[2]->value(), dst_[3]->value()};
	if (lockAct_->isChecked() && (widthChanged || heightChanged))
		r = applySizeEdit(s->dst, r.w, r.h, widthChanged, true);
	c_->setSliceDst(s->id, r);
}

QImage MappingEditor::captureImage()
{
	QPixmap pm = grab();
	QPainter p(&pm);
	for (PreviewView *v : {input_, output_}) {
		QImage img = v->capture();
		if (!img.isNull())
			p.drawImage(QRect(v->mapTo(this, QPoint(0, 0)), v->size()), img);
	}
	p.end();
	return pm.toImage();
}

void MappingEditor::keyPressEvent(QKeyEvent *e)
{
	/* Don't let Esc/Enter close the editor dialog. */
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		e->accept();
		return;
	}
	QDialog::keyPressEvent(e);
}

void MappingEditor::closeEvent(QCloseEvent *e)
{
	c_->save();
	QDialog::closeEvent(e);
}

} // namespace om
