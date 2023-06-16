// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/qseecom.h>
#include <linux/qtee_shmbridge.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <misc/qseecom_kernel.h>
#include <soc/qcom/qseecomi.h>
#include <soc/qcom/ramdump.h>
#include <soc/qcom/smcinvoke_object.h>
#include <linux/smcinvoke.h>

#include "../soc/qcom/helioscom.h"

#include "qcom_common.h"
#include "qcom_pil_info.h"
#include "remoteproc_internal.h"

#include "../soc/qcom/helios_app_smc_interface.h"

#include <soc/qcom/CAppClient.h>
#include <soc/qcom/CAppLoader.h>
#include <soc/qcom/IAppClient.h>
#include <soc/qcom/IAppController.h>
#include <soc/qcom/IAppLoader.h>
#include <soc/qcom/IClientEnv.h>
#include <soc/qcom/IOpener.h>

#define RESULT_SUCCESS		0
#define RESULT_FAILURE		-1

/* Helios Full Ramdump Size 16 MB */
#define HELIOS_FULL_RAMDUMP_SZ SZ_16M

/* Helios Ramdump Buffer Size 4 MB */
#define HELIOS_RAMDUMP_BUFFER_SZ SZ_4M

#define HELIOS_APP_PARTIAL_RAMDUMP 100

uint32_t helios_app_uid = 286;
static struct workqueue_struct *helios_reset_wq;

/* tzapp command list.*/
enum helios_tz_commands {
	HELIOS_RPROC_RAMDUMP,
	HELIOS_RPROC_IMAGE_LOAD,
	HELIOS_RPROC_AUTH_MDT,
	HELIOS_RPROC_DLOAD_CONT,
	HELIOS_RPROC_GET_HELIOS_VERSION,
	HELIOS_RPROC_SHUTDOWN,
	HELIOS_RPROC_DUMPINFO,
	HELIOS_RPROC_UP_INFO,
	HELIOS_RPROC_RESTART,
	HELIOS_RPROC_POWERDOWN,
};

/* tzapp bg request.*/
struct tzapp_helios_req {
	uint64_t address_fw;
	uint64_t size_fw;
	uint32_t tzapp_helios_cmd;
} __attribute__((__packed__));

/* tzapp bg response.*/
struct tzapp_helios_rsp {
	uint32_t tzapp_helios_cmd;
	uint32_t helios_info_len;
	int32_t status;
	uint32_t helios_info[100];
} __attribute__((__packed__));

/**
 * struct pil_mdt - Representation of <name>.mdt file in memory
 * @hdr: ELF32 header
 * @phdr: ELF32 program headers
 */
struct pil_mdt {
	struct elf32_hdr hdr;
	struct elf32_phdr phdr[];
};

/**
 * struct qcom_helios
 * @dev: Device pointer
 * @rproc: Remoteproc handle for helios
 * @firmware_name: FW image file name
 * @ssr_subdev: SSR subdevice to be registered with remoteproc
 * @ssr_name: SSR subdevice name used as reference in remoteproc
 * @config_type: config handle registered with heliosCOM
 * @glink_subdev: GLINK subdevice to be registered with remoteproc
 * @sysmon: sysmon subdevice to be registered with remoteproc
 * @sysmon_name: sysmon subdevice name used as reference in remoteproc
 * @ssctl_id: instance id of the ssctl QMI service
 * @reboot_nb: notifier block to handle reboot scenarios
 * @address_fw: address where firmware binaries loaded in DMA
 * @size_fw: size of helios firmware binaries in DMA
 * @qseecom_handle: handle of TZ app
 * @cmd_status: qseecom command status
 * @app_status: status of tz app loading
 * @is_ready: Is helios chip up
 * @err_ready: The error ready signal
 * @region_start: DMA handle for loading FW
 * @region_end: DMA address indicating end of DMA buffer
 * @region: CPU address for DMA buffer
 * @is_region_allocated: Is DMA buffer allocated
 * @region_size: DMA buffer size for FW
 */
struct qcom_helios {
	struct device *dev;
	struct rproc *rproc;

	const char *firmware_name;

	struct qcom_rproc_ssr ssr_subdev;
	const char *ssr_name;

	struct helioscom_reset_config_type config_type;

	struct qcom_rproc_glink glink_subdev;
	struct qcom_sysmon *sysmon;
	const char *sysmon_name;
	int ssctl_id;

	struct notifier_block reboot_nb;
	struct work_struct reset_handler;

	phys_addr_t address_fw;
	size_t size_fw;

	struct qseecom_handle *qseecom_handle;
	u32 cmd_status;
	int app_status;
	bool is_ready;
	struct completion err_ready;

	phys_addr_t region_start;
	phys_addr_t region_end;
	void *region;
	bool is_region_allocated;
	size_t region_size;
};

static void helios_reset_handler_work(struct work_struct *work)
{
	struct qcom_helios *helios = container_of(work, struct qcom_helios, reset_handler);
	struct rproc *helios_rproc = helios->rproc;
	bool recovery_status = helios_rproc->recovery_disabled;

	pr_debug("Handle reset\n");

	/* Disable recovery to trigger shutdown sequence to power off Helios */
	mutex_lock(&helios_rproc->lock);
	if (helios_rproc->state == RPROC_CRASHED ||
			helios_rproc->state == RPROC_OFFLINE) {
		mutex_unlock(&helios_rproc->lock);
		return;
	}
	helios_rproc->recovery_disabled = true;
	mutex_unlock(&helios_rproc->lock);

	/* Shutdown helios */
	rproc_shutdown(helios_rproc);

	/* Power up and load image again on Helios */
	rproc_boot(helios_rproc);

	/* Restore the recovery value */
	mutex_lock(&helios_rproc->lock);
	helios_rproc->recovery_disabled = recovery_status;
	mutex_unlock(&helios_rproc->lock);
}

/* Callback function registered with helioscom which triggers restart flow */
static void helios_crash_handler(void *handle, void *priv,
		enum helioscom_reset_type reset_type)
{
	struct rproc *helios_rproc = (struct rproc *)priv;
	struct qcom_helios *helios = (struct qcom_helios *)helios_rproc->priv;

	dev_err(helios->dev, "%s: Reset type:[%d]\n", __func__, reset_type);

	switch (reset_type) {

	case HELIOSCOM_OEM_PROV_PASS:
	case HELIOSCOM_OEM_PROV_FAIL:
		/* Reset Helios when OEM provisioning passes */
		dev_err(helios->dev, "%s: OEM Provision [%s]! Reset Helios.\n",
				__func__, (reset_type == HELIOSCOM_OEM_PROV_PASS) ?
				"successful" : "failed");
		if (helios_reset_wq)
			queue_work(helios_reset_wq, &helios->reset_handler);
		break;

	case HELIOSCOM_HELIOS_CRASH:
		dev_err(helios->dev, "%s: Helios reset failure type:[%d]\n",
				__func__, reset_type);
		/* Trigger Aurora crash if recovery is disabled */
		BUG_ON(helios_rproc->recovery_disabled);

		/* If recovery is enabled, go for recovery path */
		pr_debug("Helios is crashed! Starting recovery...\n");
		rproc_report_crash(helios_rproc, RPROC_FATAL_ERROR);
		break;

	default:
		dev_err(helios->dev, "%s: Invalid reset type.\n", __func__);
		break;
	}
}

/**
 * load_helios_tzapp() - Called to load TZ app.
 * @pbd: struct containing private <helios> data.
 *
 * Return: 0 on success. Error code on failure.
 */
static int load_helios_tzapp(struct qcom_helios *pbd)
{
	int rc = 0;
	uint8_t *buffer  = NULL;
	struct qtee_shm shm = {0};
	size_t size = 0;
	char *app_name = "heliosapp";
	struct Object client_env = {NULL, NULL};
	struct Object app_client = {NULL, NULL};
	struct Object app_loader = {NULL, NULL};
	struct Object app_controller_obj = {NULL, NULL};

	pbd->app_status = RESULT_FAILURE;

	/* Load the APP */
	pr_debug("Start loading of secure app\n");
	rc = get_client_env_object(&client_env);
	if (rc) {
		client_env.invoke = NULL;
		client_env.context = NULL;
		dev_err(pbd->dev, "<helios> get client env object failure\n");
		rc =  -EIO;
		goto end;
	}

	rc = IClientEnv_open(client_env, CAppLoader_UID, &app_loader);
	if (rc) {
		app_loader.invoke = NULL;
		app_loader.context = NULL;
		dev_err(pbd->dev, "<helios> IClientEnv_open failure\n");
		rc = -EIO;
		goto end;
	}

	buffer = firmware_request_from_smcinvoke(app_name, &size, &shm);
	if (buffer == NULL) {
		dev_err(pbd->dev, "firmware_request_from_smcinvoke failure\n");
		rc =  -EINVAL;
		goto end;
	}

	rc = IAppLoader_loadFromBuffer(app_loader, (const void *)buffer, size,
			&app_controller_obj);
	if (rc) {
		app_controller_obj.invoke = NULL;
		app_controller_obj.context = NULL;
		dev_err(pbd->dev, "<helios> IAppLoader_loadFromBuffer failure\n");
		rc = -EIO;
		goto end;
	}

	rc = IClientEnv_open(client_env, CAppClient_UID, &app_client);
	if (rc) {
		app_client.invoke = NULL;
		app_client.context = NULL;
		dev_err(pbd->dev, "<helios> CAppClient_UID failure\n");
		rc = -EIO;
		goto end;
	}

	pbd->app_status = RESULT_SUCCESS;
end:
	Object_ASSIGN_NULL(app_controller_obj);
	Object_ASSIGN_NULL(app_loader);
	Object_ASSIGN_NULL(client_env);
	Object_ASSIGN_NULL(app_client);
	return rc;
}

static int32_t get_helios_app_object(struct Object *helios_app_obj)
{
	int32_t ret = 0;
	const char *app_name = "heliosapp";
	struct Object remote_obj = {NULL, NULL};
	struct Object client_env = {NULL, NULL};
	struct Object app_client = {NULL, NULL};

	ret = get_client_env_object(&client_env);
	if (ret) {
		client_env.invoke = NULL;
		client_env.context = NULL;
		pr_err("<helios> get client env object failure:[%d]\n", ret);
		ret =  -EIO;
		goto end;
	}

	ret = IClientEnv_open(client_env, CAppClient_UID, &app_client);
	if (ret) {
		app_client.invoke = NULL;
		app_client.context = NULL;
		pr_err("<helios> CAppClient_UID failure:[%d]\n", ret);
		ret = -EIO;
		goto end;
	}

	ret = IAppClient_getAppObject(app_client, app_name, strlen(app_name), &remote_obj);
	if (ret) {
		pr_err("IAppClient_getAppObject failure:[%d]\n", ret);
		remote_obj.invoke = NULL;
		remote_obj.context = NULL;
		ret = -EIO;
		goto end;
	}

	ret = IOpener_open(remote_obj, helios_app_uid, helios_app_obj);
	if (ret) {
		pr_err("IOpener_open failure: ret:[%d]\n", ret);
		helios_app_obj->invoke = NULL;
		helios_app_obj->context = NULL;
		ret = -EIO;
		goto end;
	}

end:
	Object_ASSIGN_NULL(remote_obj);
	Object_ASSIGN_NULL(client_env);
	Object_ASSIGN_NULL(app_client);
	return ret;
}

/**
 * helios_tzapp_comm() - Function called to communicate with TZ APP.
 * @pbd: struct containing private <helios> data.
 * @req: struct containing command and parameters.
 *
 * Return: 0 on success. Error code on failure.
 */
static long helios_tzapp_comm(struct qcom_helios *pbd,
		struct tzapp_helios_req *req)
{
	int32_t ret = 0;
	struct Object helios_app_obj = {NULL, NULL};

	pr_debug("command id = %d\n", req->tzapp_helios_cmd);
	ret = get_helios_app_object(&helios_app_obj);
	if (ret) {
		dev_err(pbd->dev, "<helios> Failed to get helios TA context\n");
		goto end;
	}

	switch (req->tzapp_helios_cmd) {

	case HELIOS_RPROC_AUTH_MDT:
		pbd->cmd_status = helios_app_load_meta_data(
				helios_app_obj,
				(void *)req,
				sizeof(struct tzapp_helios_req));

		break;

	case HELIOS_RPROC_IMAGE_LOAD:
		pbd->cmd_status = helios_app_transfer_and_authenticate_fw(
				helios_app_obj,
				(void *)req,
				sizeof(struct tzapp_helios_req));
		break;

	case HELIOS_RPROC_RAMDUMP:
		pbd->cmd_status = helios_app_collect_ramdump(
				helios_app_obj,
				(void *)req,
				sizeof(struct tzapp_helios_req));
		break;

	case HELIOS_RPROC_RESTART:
		pbd->cmd_status = helios_app_force_restart(helios_app_obj);
		break;

	case HELIOS_RPROC_SHUTDOWN:
		pbd->cmd_status = helios_app_shutdown(helios_app_obj);
		break;

	case HELIOS_RPROC_POWERDOWN:
		pbd->cmd_status = helios_app_force_power_down(helios_app_obj);
		break;

	default:
		pr_debug("Invalid command\n");
		break;
	}

end:
	Object_ASSIGN_NULL(helios_app_obj);
	return ret;
}

/**
 * helios_auth_metadata() - Called by load operation of remoteproc framework
 * send command to tz app for authentication of metadata.
 * @helios_data: struct containing private <helios> data
 * @metadata: metadata load address
 * @size: size of metadata
 *
 * Return: 0 on success. Error code on failure.
 */
static int helios_auth_metadata(struct qcom_helios *helios_data,
		const u8 *metadata, size_t size)
{
	struct tzapp_helios_req helios_tz_req;
	struct qtee_shm shm;
	int ret;

	ret = qtee_shmbridge_allocate_shm(size, &shm);
	if (ret) {
		pr_err("Shmbridge memory allocation failed\n");
		return ret;
	}

	/* Make sure there are no mappings in PKMAP and fixmap */
	kmap_flush_unused();

	memcpy(shm.vaddr, metadata, size);

	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_AUTH_MDT;
	helios_tz_req.address_fw = shm.paddr;
	helios_tz_req.size_fw = size;

	ret = helios_tzapp_comm(helios_data, &helios_tz_req);
	if (ret || helios_data->cmd_status) {
		dev_err(helios_data->dev,
				"%s: Metadata loading failed\n",
				__func__);
		ret = helios_data->cmd_status;
		goto tzapp_com_failed;
	}

	pr_debug("Metadata loaded successfully\n");

tzapp_com_failed:
	qtee_shmbridge_free_shm(&shm);
	return ret;
}

/**
 * helios_auth_and_xfer() - Called by start operation of remoteproc framework
 * to signal tz app to authenticate and boot helios chip.
 * @helios_data: struct containing private <helios> data.
 *
 * Return: 0 on success. Error code on failure.
 */
static int helios_auth_and_xfer(struct qcom_helios *helios_data)
{
	struct tzapp_helios_req helios_tz_req;
	uint32_t ns_vmids[] = {VMID_HLOS};
	uint32_t ns_vm_perms[] = {PERM_READ | PERM_WRITE};
	u64 shm_bridge_handle;
	int ret;

	ret = qtee_shmbridge_register(helios_data->address_fw, helios_data->size_fw,
			ns_vmids, ns_vm_perms, 1, PERM_READ | PERM_WRITE,
			&shm_bridge_handle);

	if (ret) {
		dev_err(helios_data->dev,
				"%s: Failed to create shm bridge [%d]\n",
				__func__, ret);
		return ret;
	}

	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_IMAGE_LOAD;
	helios_tz_req.address_fw = helios_data->address_fw;
	helios_tz_req.size_fw = helios_data->size_fw;

	ret = helios_tzapp_comm(helios_data, &helios_tz_req);
	if (ret || helios_data->cmd_status) {
		dev_err(helios_data->dev,
				"%s: Firmware image authentication failed\n",
				__func__);
		ret = helios_data->cmd_status;
		goto tzapp_comm_failed;
	}

	/* helios Transfer of image is complete, free up the memory */
	pr_debug("Firmware authentication and transfer done\n");
	helios_data->is_ready = true;

tzapp_comm_failed:
	qtee_shmbridge_deregister(shm_bridge_handle);
	return ret;
}

/**
 * helios_prepare() - Called by rproc_boot. This loads tz app.
 * @rproc: struct containing private helios data.
 *
 * Return: 0 on success. Error code on failure.
 */
static int helios_prepare(struct rproc *rproc)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	int ret = 0;

	init_completion(&helios->err_ready);
	if (helios->app_status != RESULT_SUCCESS) {
		ret = load_helios_tzapp(helios);
		if (ret) {
			dev_err(helios->dev,
					"%s: helios TZ app load failure\n",
					__func__);
			return ret;
		}
	}

	pr_debug("heliosapp loaded\n");
	return ret;
}

#define segment_is_hash(flag) (((flag) & (0x7 << 24)) == (0x2 << 24))

static int segment_is_loadable(const struct elf32_phdr *p)
{
	return (p->p_type == PT_LOAD) && !segment_is_hash(p->p_flags) &&
		p->p_memsz;
}

static bool segment_is_relocatable(const struct elf32_phdr *p)
{
	return !!(p->p_flags & BIT(27));
}

static int helios_alloc_mem(struct qcom_helios *helios, size_t aligned_size)
{
	helios->region = dma_alloc_coherent(helios->dev, aligned_size,
			&helios->region_start, GFP_KERNEL);

	if (!helios->region)
		return -ENOMEM;

	return 0;
}

static int helios_alloc_region(struct qcom_helios *helios, phys_addr_t min_addr,
		phys_addr_t max_addr, size_t align)
{
	size_t size = max_addr - min_addr;
	size_t aligned_size;
	int ret;

	/* Don't reallocate due to fragmentation concerns, just sanity check */
	if (helios->is_region_allocated) {
		if (WARN(helios->region_end - helios->region_start < size,
					"Can't reuse PIL memory, too small\n"))
			return -ENOMEM;
		return 0;
	}

	if (align >= SZ_4M)
		aligned_size = ALIGN(size, SZ_4M);
	else if (align >= SZ_1M)
		aligned_size = ALIGN(size, SZ_1M);
	else
		aligned_size = ALIGN(size, SZ_4K);

	ret = helios_alloc_mem(helios, aligned_size);
	if (ret) {
		dev_err(helios->dev,
				"%s: Failed to allocate relocatable region\n",
				__func__);
		helios->region_start = 0;
		helios->region_end = 0;
		return ret;
	}

	helios->is_region_allocated = true;
	helios->region_end = helios->region_start + size;
	helios->region_size = aligned_size;

	return 0;
}

static int helios_setup_region(struct qcom_helios *helios, const struct pil_mdt *mdt)
{
	const struct elf32_phdr *phdr;
	phys_addr_t min_addr_r, min_addr_n, max_addr_r, max_addr_n, start, end;
	size_t align = 0;
	int i, ret = 0;
	bool relocatable = false;

	min_addr_n = min_addr_r = (phys_addr_t)ULLONG_MAX;
	max_addr_n = max_addr_r = 0;

	/* Find the image limits */
	for (i = 0; i < mdt->hdr.e_phnum; i++) {
		phdr = &mdt->phdr[i];
		if (!segment_is_loadable(phdr))
			continue;

		start = phdr->p_paddr;
		end = start + phdr->p_memsz;

		if (segment_is_relocatable(phdr)) {
			min_addr_r = min(min_addr_r, start);
			max_addr_r = max(max_addr_r, end);
			/*
			 * Lowest relocatable segment dictates alignment of
			 * relocatable region
			 */
			if (min_addr_r == start)
				align = phdr->p_align;
			relocatable = true;
		} else {
			min_addr_n = min(min_addr_n, start);
			max_addr_n = max(max_addr_n, end);
		}
	}

	/*
	 * Align the max address to the next 4K boundary to satisfy iommus and
	 * XPUs that operate on 4K chunks.
	 */
	max_addr_n = ALIGN(max_addr_n, SZ_4K);
	max_addr_r = ALIGN(max_addr_r, SZ_4K);

	if (relocatable) {
		ret = helios_alloc_region(helios, min_addr_r, max_addr_r, align);
	} else {
		helios->region_start = min_addr_n;
		helios->region_end = max_addr_n;
	}

	return ret;
}

static int helios_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	int ret = 0;
	const struct pil_mdt *mdt;

	mdt = (const struct pil_mdt *)fw->data;
	ret = helios_setup_region(helios, mdt);
	if (ret) {
		dev_err(helios->dev, "%s: helios memory setup failure\n", __func__);
		return ret;
	}
	pr_debug("Loading from %pa to %pa\n", &helios->region_start,
			&helios->region_end);

	ret = qcom_mdt_load_no_init(helios->dev, fw, rproc->firmware, 0,
			helios->region, helios->region_start, helios->region_size,
			NULL);
	if (ret) {
		dev_err(helios->dev, "%s: helios memory setup failure\n", __func__);
		return ret;
	}

	/* Send the metadata */
	ret = helios_auth_metadata(helios, fw->data, fw->size);
	if (ret) {
		dev_err(helios->dev, "%s: helios TZ app load failure\n", __func__);
		return ret;
	}

	return 0;
}

static int helios_start(struct rproc *rproc)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	int ret = 0;

	helios->address_fw = helios->region_start;
	helios->size_fw = helios->region_end - helios->region_start;
	ret = helios_auth_and_xfer(helios);
	if (ret) {
		dev_err(helios->dev, "%s: helios TZ app load failure\n", __func__);
		return ret;
	}

	pr_debug("Helios is booted up!\n");

	dma_free_coherent(helios->dev, helios->region_size, helios->region,
			helios->region_start);
	helios->is_region_allocated = false;
	helios->region = NULL;
	helios->region_start = 0;
	helios->region_end = 0;
	helios->region_size = 0;

	return 0;
}

static void dumpfn(struct rproc *rproc, struct rproc_dump_segment *segment,
		void *dest, size_t offset, size_t size)
{
	if (segment)
		memcpy(dest, segment->priv, size);
	pr_debug("Dump Segment Added\n");
}

static void helios_coredump(struct rproc *rproc)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	struct tzapp_helios_req helios_tz_req;
	uint32_t ns_vmids[] = {VMID_HLOS};
	uint32_t ns_vm_perms[] = {PERM_READ | PERM_WRITE};
	u64 shm_bridge_handle;
	phys_addr_t start_addr;
	void *region;
	void *full_ramdump_buffer = NULL;
	int ret;
	unsigned long size = HELIOS_RAMDUMP_BUFFER_SZ;
	size_t buffer_size = 0;

	rproc_coredump_cleanup(rproc);

	if (!helios->is_ready) {
		dev_err(helios->dev, "%s: Helios is not up! Returning..\n", __func__);
		return;
	}

	region = dma_alloc_attrs(helios->dev, size,
				&start_addr, GFP_KERNEL, DMA_ATTR_SKIP_ZEROING);
	if (region == NULL) {
		dev_err(helios->dev,
			"Allocation of ramdump region failed. Try with reduced size\n");
		size = size / 2;
		region = dma_alloc_attrs(helios->dev, size,
					&start_addr, GFP_KERNEL, DMA_ATTR_SKIP_ZEROING);
		if (region == NULL) {
			dev_err(helios->dev,
				"Helios failure to allocate ramdump region of size %zx\n",
				size);
			return;
		}
	}

	ret = qtee_shmbridge_register(start_addr, size,
		ns_vmids, ns_vm_perms, 1, PERM_READ|PERM_WRITE,
		&shm_bridge_handle);
	if (ret) {
		dev_err(helios->dev,
				"%s: Failed to create shm bridge. ret=[%d]\n",
				__func__, ret);
		goto dma_free;
	}

	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_RAMDUMP;
	helios_tz_req.address_fw = start_addr;
	helios_tz_req.size_fw = size;

	full_ramdump_buffer = vmalloc(HELIOS_FULL_RAMDUMP_SZ);
	if (full_ramdump_buffer == NULL)
		goto shm_free;

	do {
		ret = helios_tzapp_comm(helios, &helios_tz_req);
		if (ret) {
			dev_err(helios->dev, "%s: Helios Ramdump collection failed:[%d]\n",
					__func__, ret);
			goto exit;
		}
		if (helios->cmd_status &&
				helios->cmd_status != HELIOS_APP_PARTIAL_RAMDUMP) {
			dev_err(helios->dev, "%s: Helios Ramdump collection failed:[%d]\n",
					__func__, helios->cmd_status);
			goto exit;
		}
		memcpy(full_ramdump_buffer + buffer_size, region, size);
		buffer_size += size;
	} while (helios->cmd_status == HELIOS_APP_PARTIAL_RAMDUMP);

	pr_debug("Add coredump segment!\n");
	ret = rproc_coredump_add_custom_segment(rproc, start_addr, buffer_size,
			&dumpfn, full_ramdump_buffer);
	if (ret) {
		dev_err(helios->dev, "failed to add rproc segment: %d\n", ret);
		rproc_coredump_cleanup(helios->rproc);
		goto exit;
	}

	/* Prepare coredump file */
	rproc_coredump(rproc);

exit:
	vfree(full_ramdump_buffer);
shm_free:
	qtee_shmbridge_deregister(shm_bridge_handle);
dma_free:
	dma_free_attrs(helios->dev, size, region,
			   start_addr, DMA_ATTR_SKIP_ZEROING);
}

static int helios_force_powerdown(struct qcom_helios *helios)
{
	int ret;
	struct tzapp_helios_req helios_tz_req;

	pr_debug("Force powerdown helios\n");
	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_POWERDOWN;
	helios_tz_req.address_fw = 0;
	helios_tz_req.size_fw = 0;
	ret = helios_tzapp_comm(helios, &helios_tz_req);
	if (ret || helios->cmd_status) {
		dev_err(helios->dev, "%s: Helios Power Down failed\n", __func__);
		return helios->cmd_status;
	}

	helios->is_ready = false;

	pr_debug("Helios is powered down.\n");
	return ret;
}

static int helios_force_restart(struct rproc *rproc)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	struct tzapp_helios_req helios_tz_req;
	int ret = RESULT_FAILURE;

	helios->cmd_status = 0;

	if (!helios->is_ready) {
		dev_err(helios->dev, "%s: Helios is not up!\n", __func__);
		ret = RESULT_FAILURE;
		goto end;
	}

	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_RESTART;
	helios_tz_req.address_fw = 0;
	helios_tz_req.size_fw = 0;

	ret = helios_tzapp_comm(helios, &helios_tz_req);
	if (!ret && !helios->cmd_status) {
		/* Helios Restart is success but Helios is responding.
		 * In this case, collect ramdump and store it.
		 */
		pr_info("A2H response is received! Collect ramdump now!\n");
		helios_coredump(rproc);

		pr_debug("Helios Restart is success!\n");
		helios->is_ready = false;
	}

end:
	if (ret || helios->cmd_status) {
		/* Helios is not responding. So forcing S3 reset to power down Helios
		 * and Yoda. It should be powered on in Start Sequence.
		 */
		pr_debug("Helios is not responding. Powerdown helios\n");
		ret = helios_force_powerdown(helios);
		if (ret) {
			dev_err(helios->dev, "%s: Helios Power Down failed\n", __func__);
			return ret;
		}
		pr_debug("Helios Force Power Down is success!\n");
		helios->is_ready = false;
	}
	return ret;
}

static int helios_shutdown(struct rproc *rproc)
{
	struct qcom_helios *helios = rproc->priv;
	struct tzapp_helios_req helios_tz_req;
	int ret = RESULT_FAILURE;

	helios->cmd_status = 0;

	if (!helios->is_ready) {
		dev_err(helios->dev, "%s: Helios is not up!\n", __func__);
		ret = RESULT_FAILURE;
		goto end;
	}

	helios_tz_req.tzapp_helios_cmd = HELIOS_RPROC_SHUTDOWN;
	helios_tz_req.address_fw = 0;
	helios_tz_req.size_fw = 0;

	ret = helios_tzapp_comm(helios, &helios_tz_req);
	if (ret || helios->cmd_status) {
		pr_debug("Helios is not responding. Powerdown helios\n");
		goto end;
	}

	helios->is_ready = false;
	pr_debug("Helios Shutdown is success!\n");
	return ret;

end:
	if (ret || helios->cmd_status) {
		/* Helios is not responding. So forcing S3 reset to power down Helios
		 * and Yoda. It should be powered on in Start Sequence.
		 */
		ret = helios_force_powerdown(helios);
		if (ret) {
			dev_err(helios->dev, "%s: Helios Power Down failed\n", __func__);
			return ret;
		}
		pr_debug("Helios Force Power Down is success!\n");
		helios->is_ready = false;
	}
	return ret;
}

/**
 * helios_stop() - Called by stop operation of remoteproc framework
 * It can help to force restart or shutdown Helios based on recovery option.
 * @rproc: struct containing private helios data.
 *
 * Return: success or TA response error code
 */
static int helios_stop(struct rproc *rproc)
{
	struct qcom_helios *helios = (struct qcom_helios *)rproc->priv;
	int ret = 0;

	/* In case of crash, STOP operation is dummy */
	if (rproc->state == RPROC_CRASHED) {
		pr_err("Helios is crashed! Skip stop and collect ramdump directly.\n");
		return ret;
	}

	if (helios->is_ready) {
		if (rproc->recovery_disabled)
			ret = helios_shutdown(rproc);
		else
			ret = helios_force_restart(rproc);
	}

	pr_info("Helios Stop is %s\n", ret ? "failed" : "success");
	return ret;
}

static const struct rproc_ops helios_ops = {
	.prepare = helios_prepare,
	.get_boot_addr = rproc_elf_get_boot_addr,
	.load = helios_load,
	.start = helios_start,
	.stop = helios_stop,
	.coredump = helios_coredump,
};

static int helios_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct qcom_helios *helios = container_of(nb, struct qcom_helios, reboot_nb);

	pr_debug("System is going for reboot!. Shutdown helios.\n");
	helios->rproc->recovery_disabled = true;
	rproc_shutdown(helios->rproc);
	pr_debug("Helios is Shutdown successfully.\n");

	return NOTIFY_OK;
}

static int rproc_helios_driver_probe(struct platform_device *pdev)
{
	struct qcom_helios *helios;
	struct rproc *rproc;
	const char *fw_name;
	const char *ssr_name;
	void *config_handle = NULL;
	int ret;

	ret = of_property_read_string(pdev->dev.of_node,
			"qcom,firmware-name", &fw_name);
	if (ret)
		return ret;

	ret = of_property_read_string(pdev->dev.of_node,
			"qcom,ssr-name", &ssr_name);
	if (ret)
		return ret;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &helios_ops,
			fw_name, sizeof(*helios));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);

	helios = (struct qcom_helios *)rproc->priv;
	helios->ssr_name = ssr_name;
	helios->firmware_name = fw_name;
	helios->dev = &pdev->dev;
	helios->app_status = RESULT_FAILURE;
	rproc->dump_conf = RPROC_COREDUMP_ENABLED;
	rproc->recovery_disabled = true;
	rproc->auto_boot = false;
	helios->rproc = rproc;
	helios->sysmon_name = "helios";
	helios->ssctl_id = 0x1d;
	platform_set_drvdata(pdev, helios);

	qcom_add_ssr_subdev(rproc, &helios->ssr_subdev, helios->ssr_name);

	qcom_add_glink_subdev(rproc, &helios->glink_subdev, helios->ssr_name);

	helios->sysmon = qcom_add_sysmon_subdev(rproc, helios->sysmon_name,
			helios->ssctl_id);
	if (IS_ERR(helios->sysmon)) {
		ret = PTR_ERR(helios->sysmon);
		dev_err(helios->dev, "%s: Error while adding sysmon subdevice:[%d]\n",
				__func__, ret);
		goto free_rproc;
	}

	/* Register callback for Helios Crash with heliosCom */
	helios->config_type.priv = (void *)rproc;
	helios->config_type.helioscom_reset_notification_cb = helios_crash_handler;
	config_handle = helioscom_pil_reset_register(&helios->config_type);
	if (!config_handle) {
		ret = -ENOMEM;
		dev_err(helios->dev, "%s: Invalid Handle\n", __func__);
		goto free_rproc;
	}

	/* Register callback for handling reboot */
	helios->reboot_nb.notifier_call = helios_reboot_notify;
	register_reboot_notifier(&helios->reboot_nb);

	helios_reset_wq = alloc_workqueue("helios_reset_wq",
						WQ_UNBOUND | WQ_FREEZABLE, 0);
	if (!helios_reset_wq) {
		dev_err(helios->dev, "%s: creation of helios_reset_wq failed\n",
				__func__);
		goto unregister_notify;
	}

	/* Initialize work queue for reset handler */
	INIT_WORK(&helios->reset_handler, helios_reset_handler_work);

	/* Register with rproc */
	ret = rproc_add(rproc);
	if (ret)
		goto destroy_wq;

	pr_debug("Helios probe is completed\n");
	return 0;

destroy_wq:
	destroy_workqueue(helios_reset_wq);
unregister_notify:
	unregister_reboot_notifier(&helios->reboot_nb);
free_rproc:
	rproc_free(rproc);

	return ret;
}

static int rproc_helios_driver_remove(struct platform_device *pdev)
{
	struct qcom_helios *helios = platform_get_drvdata(pdev);

	if (helios_reset_wq)
		destroy_workqueue(helios_reset_wq);
	unregister_reboot_notifier(&helios->reboot_nb);
	rproc_del(helios->rproc);
	rproc_free(helios->rproc);

	return 0;
}

static const struct of_device_id rproc_helios_match_table[] = {
	{.compatible = "qcom,rproc-helios"},
	{}
};
MODULE_DEVICE_TABLE(of, rproc_helios_match_table);

static struct platform_driver rproc_helios_driver = {
	.probe = rproc_helios_driver_probe,
	.remove = rproc_helios_driver_remove,
	.driver = {
		.name = "qcom-rproc-helios",
		.of_match_table = rproc_helios_match_table,
	},
};

module_platform_driver(rproc_helios_driver);
MODULE_DESCRIPTION("Support for booting QTI helios SoC");
MODULE_LICENSE("GPL v2");
