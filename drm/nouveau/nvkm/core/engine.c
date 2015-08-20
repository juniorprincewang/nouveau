/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include <core/engine.h>
#include <core/device.h>
#include <core/option.h>

void
nvkm_engine_unref(struct nvkm_engine **pengine)
{
	struct nvkm_engine *engine = *pengine;
	if (engine) {
		mutex_lock(&engine->subdev.mutex);
		if (--engine->usecount == 0)
			nvkm_subdev_fini(&engine->subdev, false);
		mutex_unlock(&engine->subdev.mutex);
		*pengine = NULL;
	}
}

struct nvkm_engine *
nvkm_engine_ref(struct nvkm_engine *engine)
{
	if (engine) {
		mutex_lock(&engine->subdev.mutex);
		if (++engine->usecount == 1) {
			int ret = nvkm_subdev_init(&engine->subdev);
			if (ret) {
				engine->usecount--;
				mutex_unlock(&engine->subdev.mutex);
				return ERR_PTR(ret);
			}
		}
		mutex_unlock(&engine->subdev.mutex);
	}
	return engine;
}

static void
nvkm_engine_intr(struct nvkm_subdev *obj)
{
	struct nvkm_engine *engine = container_of(obj, typeof(*engine), subdev);
	if (engine->func->intr)
		engine->func->intr(engine);
}

static int
nvkm_engine_fini(struct nvkm_subdev *obj, bool suspend)
{
	struct nvkm_engine *engine = container_of(obj, typeof(*engine), subdev);
	if (engine->subdev.object.oclass)
		return engine->subdev.object.oclass->ofuncs->fini(&engine->subdev.object, suspend);
	if (engine->func->fini)
		return engine->func->fini(engine, suspend);
	return 0;
}

static int
nvkm_engine_init(struct nvkm_subdev *obj)
{
	struct nvkm_engine *engine = container_of(obj, typeof(*engine), subdev);
	struct nvkm_subdev *subdev = &engine->subdev;
	int ret = 0;
	s64 time;

	if (!engine->usecount) {
		nvkm_trace(subdev, "init skipped, engine has no users\n");
		return ret;
	}

	if (engine->subdev.object.oclass)
		return engine->subdev.object.oclass->ofuncs->init(&engine->subdev.object);

	if (engine->func->oneinit && !engine->subdev.oneinit) {
		nvkm_trace(subdev, "one-time init running...\n");
		time = ktime_to_us(ktime_get());
		ret = engine->func->oneinit(engine);
		if (ret) {
			nvkm_trace(subdev, "one-time init failed, %d\n", ret);
			return ret;
		}

		engine->subdev.oneinit = true;
		time = ktime_to_us(ktime_get()) - time;
		nvkm_trace(subdev, "one-time init completed in %lldus\n", time);
	}

	if (engine->func->init)
		ret = engine->func->init(engine);

	return ret;
}

static void *
nvkm_engine_dtor(struct nvkm_subdev *obj)
{
	struct nvkm_engine *engine = container_of(obj, typeof(*engine), subdev);
	if (engine->subdev.object.oclass) {
		engine->subdev.object.oclass->ofuncs->dtor(&engine->subdev.object);
		return NULL;
	}
	if (engine->func->dtor)
		return engine->func->dtor(engine);
	return engine;
}

static const struct nvkm_subdev_func
nvkm_engine_func = {
	.dtor = nvkm_engine_dtor,
	.init = nvkm_engine_init,
	.fini = nvkm_engine_fini,
	.intr = nvkm_engine_intr,
};

int
nvkm_engine_ctor(const struct nvkm_engine_func *func,
		 struct nvkm_device *device, int index, u32 pmc_enable,
		 bool enable, struct nvkm_engine *engine)
{
	nvkm_subdev_ctor(&nvkm_engine_func, device, index,
			 pmc_enable, &engine->subdev);
	engine->func = func;

	if (!nvkm_boolopt(device->cfgopt, nvkm_subdev_name[index], enable)) {
		nvkm_debug(&engine->subdev, "disabled\n");
		return -ENODEV;
	}

	spin_lock_init(&engine->lock);
	return 0;
}

int
nvkm_engine_new_(const struct nvkm_engine_func *func,
		 struct nvkm_device *device, int index, u32 pmc_enable,
		 bool enable, struct nvkm_engine **pengine)
{
	if (!(*pengine = kzalloc(sizeof(**pengine), GFP_KERNEL)))
		return -ENOMEM;
	return nvkm_engine_ctor(func, device, index, pmc_enable,
				enable, *pengine);
}

struct nvkm_engine *
nvkm_engine(void *obj, int idx)
{
	obj = nvkm_subdev(obj, idx);
	if (obj && nv_iclass(obj, NV_ENGINE_CLASS))
		return nv_engine(obj);
	return NULL;
}

int
nvkm_engine_create_(struct nvkm_object *parent, struct nvkm_object *engobj,
		    struct nvkm_oclass *oclass, bool enable,
		    const char *iname, const char *fname,
		    int length, void **pobject)
{
	struct nvkm_engine *engine;
	int ret;

	ret = nvkm_subdev_create_(parent, engobj, oclass, NV_ENGINE_CLASS,
				  iname, fname, length, pobject);
	engine = *pobject;
	if (ret)
		return ret;

	if (parent) {
		struct nvkm_device *device = nv_device(parent);
		int engidx = nv_engidx(engine);

		if (device->disable_mask & (1ULL << engidx)) {
			if (!nvkm_boolopt(device->cfgopt, iname, false)) {
				nvkm_debug(&engine->subdev,
					   "engine disabled by hw/fw\n");
				return -ENODEV;
			}

			nvkm_warn(&engine->subdev,
				  "ignoring hw/fw engine disable\n");
		}

		if (!nvkm_boolopt(device->cfgopt, iname, enable)) {
			if (!enable)
				nvkm_warn(&engine->subdev,
					  "disabled, %s=1 to enable\n", iname);
			return -ENODEV;
		}
	}

	INIT_LIST_HEAD(&engine->contexts);
	spin_lock_init(&engine->lock);
	engine->subdev.func = &nvkm_engine_func;
	return 0;
}
