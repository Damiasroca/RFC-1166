#pragma once

#include "kerry_cam_exposure_cfg.h"

void kerryCamExpStoreGet(kerry_cam_exp_cfg_t *out);
void kerryCamExpStoreSetStaged(const kerry_cam_exp_cfg_t *cfg);

void kerryCamExposureLoad();
void kerryCamExposureSave();
void kerryCamExposureActivateStaged();
bool kerryCamExposurePersistStaged();
bool kerryCamExposureRollbackStaged();
void kerryCamExposureNoteCaptureOk();
void kerryCamExposureNoteCaptureFail();
bool kerryCamExposureRevertPending();

void kerryCamExpStoreNoteApplied();
void kerryCamExpStoreMarkApplied();
void kerryCamExpStoreClearRevertPending();
