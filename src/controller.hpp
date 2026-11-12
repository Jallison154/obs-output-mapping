/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "model/mapping.hpp"
#include "platform/platform.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace om {

class OutputWindow;

enum class OutputMode { None, Fullscreen, Window };

struct DisplayStatus {
	MatchStatus match = MatchStatus::Unassigned;
	int screen = -1;             /* index into Controller::screens() when connected */
	std::vector<int> candidates; /* ambiguous candidates */
	QString text;                /* short human readable status */
	bool exactPixels = true;     /* backing store == display mode pixels */
	bool canFullscreen = false;
};

enum class Change {
	Structure, /* displays/slices added, removed, renamed, reordered, reassigned */
	Geometry,  /* slice rectangles / display size only */
};

class Controller : public QObject {
	Q_OBJECT

public:
	static Controller *instance();
	static void create();
	static void destroy();

	const MappingDocument &doc() const { return doc_; }

	/* -- selection (shared by sidebar and both previews) -- */
	QString selectedDisplay() const { return selDisplay_; }
	QString selectedSlice() const { return selSlice_; }
	void select(const QString &displayId, const QString &sliceId);
	void selectSlice(const QString &sliceId);

	/* -- connected screens & status -- */
	const std::vector<ScreenInfo> &screens() const { return screens_; }
	void refreshScreens(); /* rescan + resume wanted outputs */
	void rescanScreens();  /* rescan only (never starts outputs) */
	DisplayStatus status(const QString &displayId) const;
	void programSize(int &w, int &h) const
	{
		w = programW_;
		h = programH_;
	}

	/* -- display mutations -- */
	QString addPhysicalDisplay(int screenIndex);
	QString addVirtualDisplay(const QString &name, int w, int h);
	void removeDisplay(const QString &id);
	void renameDisplay(const QString &id, const QString &name);
	void assignScreen(const QString &displayId, int screenIndex); /* -1 = make virtual */
	void setVirtualSize(const QString &id, int w, int h);
	void setDisplayDelay(const QString &id, int delayMs);
	void moveDisplay(const QString &id, int newIndex);

	/* -- slice mutations -- */
	QString createSlice(const QString &displayId, const IRect &src);
	QString duplicateSlice(const QString &sliceId);
	void deleteSlice(const QString &sliceId);
	void renameSlice(const QString &sliceId, const QString &name);
	void setSliceEnabled(const QString &sliceId, bool enabled);
	void setSliceSrc(const QString &sliceId, const IRect &r);
	void setSliceDst(const QString &sliceId, const IRect &r);
	void moveSlice(const QString &sliceId, const QString &toDisplayId, int index);
	void fitSliceToDisplay(const QString &sliceId);
	void sliceOneToOne(const QString &sliceId);

	void replaceDocument(const MappingDocument &doc);

	/* -- layout presets / files --
	 * Applying a layout keeps running outputs whose display still exists in
	 * the new layout (same id and physical identity), stops the others and
	 * never starts new ones. Undoable. */
	bool applyLayout(const MappingDocument &doc, const QString &presetName = QString());
	QString presetsDir() const;
	QStringList presetNames() const;
	bool savePreset(const QString &name, QString *error = nullptr);
	bool loadPreset(const QString &name, QString *error = nullptr);
	bool deletePreset(const QString &name, QString *error = nullptr);
	bool exportLayout(const QString &path, QString *error = nullptr) const;
	bool importLayout(const QString &path, QString *error = nullptr);
	QString currentPreset() const { return currentPreset_; }
	bool presetModified() const { return presetModified_; }

	/* -- undo / redo (document edits; drags coalesce into one step) -- */
	bool canUndo() const { return !undo_.empty(); }
	bool canRedo() const { return !redo_.empty(); }
	void undo();
	void redo();
	void setRestoreOutputs(bool on);
	void setExcludeFromCapture(bool on);

	/* -- outputs -- */
	bool startFullscreen(const QString &displayId, QString *error = nullptr);
	bool openWindow(const QString &displayId, QString *error = nullptr);
	void stopOutput(const QString &displayId, bool userInitiated = true);
	void stopAll();
	void startAll();
	OutputMode outputMode(const QString &displayId) const;
	int activeOutputCount() const { return (int)outputs_.size(); }

	/* Offscreen render of exactly what the display receives. */
	QImage snapshot(const QString &displayId, QString *error = nullptr);

	/* -- persistence -- */
	QString configPath() const;
	void load();
	void save();
	void restoreOutputsOnStartup();
	void shutdown();

	QJsonObject stateJson() const;

signals:
	void documentChanged(om::Change kind);
	void selectionChanged();
	void screensChanged();
	void outputsChanged();
	void programSizeChanged();
	void undoStateChanged();
	void presetsChanged();

private:
	explicit Controller(QObject *parent = nullptr);
	~Controller() override;

	void changed(Change kind);
	void recomputeStatuses();
	void resumeWantedOutputs();
	void onScreenRemoved(class QScreen *s);
	void verifyOutputs(); /* watchdog: outputs must sit exactly on their display */
	OutputWindowOptions optionsFor(QScreen *target) const;
	DisplayIdentity identityFor(const ScreenInfo &si) const { return si.identity; }

	MappingDocument doc_;
	QString selDisplay_, selSlice_;
	std::vector<ScreenInfo> screens_;
	QHash<QString, DisplayStatus> statuses_;
	QHash<QString, QPointer<OutputWindow>> outputs_;
	QTimer saveTimer_, programTimer_, screenTimer_, watchdog_;
	int programW_ = 0, programH_ = 0;
	bool loaded_ = false;
	bool shuttingDown_ = false;
	bool resuming_ = false;

	void restoreSnapshot(const MappingDocument &d);
	std::vector<MappingDocument> undo_, redo_;
	MappingDocument committed_;
	qint64 lastGeometryEdit_ = 0;
	/* Displays without a hardware identity, confirmed by the user for this
	 * session only: mapping id -> OS screen id (cleared on disconnect). */
	QHash<QString, QString> sessionConfirmed_;
	QString currentPreset_;
	bool presetModified_ = false;
	QString presetPath(const QString &name) const;
};

} // namespace om
