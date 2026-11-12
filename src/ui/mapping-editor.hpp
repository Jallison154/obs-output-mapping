/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "controller.hpp"

#include <QDialog>
#include <QTreeWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QFrame;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;

namespace om {

class PreviewView;

/* Sidebar tree with drag-and-drop reorder / reassignment of slices. */
class SliceTree : public QTreeWidget {
	Q_OBJECT
public:
	explicit SliceTree(QWidget *parent = nullptr);

protected:
	void dropEvent(QDropEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
};

class MappingEditor : public QDialog {
	Q_OBJECT

public:
	explicit MappingEditor(QWidget *parent = nullptr);
	~MappingEditor() override;

	PreviewView *inputView() const { return input_; }
	PreviewView *outputView() const { return output_; }

	/* Screenshot of the whole editor. Native video views are not captured
	 * by QWidget::grab(), so each preview is rendered offscreen through its
	 * own draw path and composited in place. */
	QImage captureImage();

protected:
	void closeEvent(QCloseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void buildUi();
	QWidget *buildPreviewPanel(PreviewView *view, QLabel *&title, QLabel *&subtitle, QLabel *&zoomLabel);
	QWidget *buildEmptyState();
	QWidget *buildInspector();

	void rebuildTree();
	void syncTreeSelection();
	void rebuildScreenCombo();
	void updateInspector();
	void updateHeaders();
	void updateActions();
	void updateStatusBar();

	void onTreeSelection();
	void onTreeItemChanged(QTreeWidgetItem *item, int column);
	void showTreeMenu(const QPoint &pos);
	void populateAddDisplayMenu(QMenu *m);
	void addVirtualDisplay();
	void addSliceToSelected();
	void startFullscreenSelected();
	void toggleWindowSelected();
	void snapshotSelected();
	void moveSelectedSlice(int delta);
	void pushSrcFromFields();
	void pushDstFromFields(bool widthChanged, bool heightChanged);

	Controller *c_;
	QStackedWidget *sideStack_ = nullptr;
	SliceTree *tree_ = nullptr;
	PreviewView *input_ = nullptr;
	PreviewView *output_ = nullptr;
	QLabel *inTitle_ = nullptr, *inSub_ = nullptr, *inZoom_ = nullptr, *targetChip_ = nullptr;
	QLabel *outTitle_ = nullptr, *outSub_ = nullptr, *outZoom_ = nullptr;
	QLabel *statusLeft_ = nullptr, *statusRight_ = nullptr;

	QToolButton *addDisplayBtn_ = nullptr;
	QAction *addSliceAct_ = nullptr, *dupAct_ = nullptr, *delAct_ = nullptr;
	QAction *undoAct_ = nullptr, *redoAct_ = nullptr;
	QAction *fitAct_ = nullptr, *oneAct_ = nullptr, *snapAct_ = nullptr, *lockAct_ = nullptr;
	QAction *restoreAct_ = nullptr, *captureAct_ = nullptr;

	/* slice inspector (shown first: it is what you edit most) */
	QGroupBox *sliceBox_ = nullptr;
	QLabel *sliceSwatch_ = nullptr;
	QLineEdit *sliceName_ = nullptr;
	QCheckBox *sliceEnabled_ = nullptr;
	QComboBox *sliceDisplay_ = nullptr;
	QSpinBox *src_[4] = {};
	QSpinBox *dst_[4] = {};
	QCheckBox *lockCheck_ = nullptr;
	QLabel *scaleLabel_ = nullptr;

	/* display inspector */
	QGroupBox *displayBox_ = nullptr;
	QLineEdit *displayName_ = nullptr;
	QComboBox *screenCombo_ = nullptr;
	QLabel *displayStatus_ = nullptr;
	QWidget *virtRow_ = nullptr;
	QSpinBox *virtW_ = nullptr, *virtH_ = nullptr;
	QLabel *resLabel_ = nullptr;
	QPushButton *fullscreenBtn_ = nullptr, *windowBtn_ = nullptr, *snapshotBtn_ = nullptr, *removeDisplayBtn_ = nullptr;
	QLabel *noSelection_ = nullptr;

	bool updating_ = false;
};

} // namespace om
