#pragma once

#include "kerry_cam_exposure_cfg.h"

void kerryCamExpStoreGet(kerry_cam_exp_cfg_t *out);
void kerryCamExpStoreSetStaged(const kerry_cam_exp_cfg_t *cfg);
bool kerryCamExpStoreGetNight(kerry_cam_exp_cfg_t *out);
void kerryCamExpStoreSetNight(const kerry_cam_exp_cfg_t *cfg);
bool kerryCamExpStoreReplace(const kerry_cam_exp_cfg_t *cfg, kerry_cam_exp_cfg_t *prev);

typedef struct {
  uint8_t version;
  kerry_cam_exp_cfg_t cfg;
} kerry_cam_exp_preset_blob_t;

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
