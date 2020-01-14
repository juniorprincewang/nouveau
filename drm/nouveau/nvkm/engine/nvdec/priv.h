/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_NVDEC_PRIV_H__
#define __NVKM_NVDEC_PRIV_H__
#include <engine/nvdec.h>

struct nvkm_nvdec_func {
};

struct nvkm_nvdec_fwif {
	int version;
	int (*load)(struct nvkm_nvdec *, int ver,
		    const struct nvkm_nvdec_fwif *);
	const struct nvkm_nvdec_func *func;
};

int nvkm_nvdec_new_(const struct nvkm_nvdec_fwif *fwif,
		    struct nvkm_device *, int, struct nvkm_nvdec **);
#endif
