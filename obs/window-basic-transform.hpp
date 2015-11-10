#pragma once

#include <obs.hpp>
#include <memory>

#include "ui_OBSBasicTransform.h"

class OBSBasic;

class OBSBasicTransform : public QDialog {
	Q_OBJECT

private:
	std::unique_ptr<Ui::OBSBasicTransform> ui;

	OBSBasic     *main;
	OBSInput     item;
	OBSSignal    channelChangedSignal;
	OBSSignal    transformSignal;
	OBSSignal    removeSignal;
	OBSSignal    selectSignal;
	OBSSignal    deselectSignal;

	bool         ignoreTransformSignal = false;
	bool         ignoreItemChange      = false;

	void HookWidget(QWidget *widget, const char *signal, const char *slot);

	void SetScene(OBSScene scene);
	void SetItem(OBSInput newItem);

	static void OBSChannelChanged(void *param, calldata_t *data);

	static void OBSInputTransform(void *param, calldata_t *data);
	static void OBSInputRemoved(void *param, calldata_t *data);
	static void OBSInputSelect(void *param, calldata_t *data);
	static void OBSInputDeselect(void *param, calldata_t *data);

private slots:
	void RefreshControls();
	void SetItemQt(OBSInput newItem);
	void OnBoundsType(int index);
	void OnControlChanged();

public:
	OBSBasicTransform(OBSBasic *parent);
};
