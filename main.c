// SPDX-License-Identifier: GPL-2.0
/*
 * KernelSpace Profiles
 *
 * This Linux kernel module provides a framework for managing and switching between system profiles or modes
 * at the kernel level. Each profile represents a specific configuration of kernel features and settings
 * optimized for different use cases such as battery life, balanced performance, or maximum performance.
 *
 * The module supports various subsystems, including MSM DRM, MI DRM, and framebuffer (FB). It integrates
 * with these subsystems to receive notifications about screen state changes and adjust the active profile
 * accordingly.
 *
 * The module offers functions for setting the profile mode, overriding the mode temporarily, and retrieving
 * the active profile mode. Profiles can be dynamically switched based on system events, user requests, or
 * time-based rules.
 *
 * For more information and usage examples, refer to the README file at:
 * https://github.com/beakthoven/Kprofiles/blob/main/README.md
 *
 * Copyright (C) 2021-2025 Dakkshesh <dakkshesh5@gmail.com>
 * Version: 6.0.0
 * License: GPL-2.0
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#ifdef CONFIG_AUTO_KPROFILES_MSM_DRM
#include <linux/msm_drm_notify.h>
#elif defined(CONFIG_AUTO_KPROFILES_MI_DRM)
#include <drm/drm_notifier_mi.h>
#elif defined(CONFIG_AUTO_KPROFILES_FB)
#include <linux/fb.h>
#endif
#include "version.h"
#include <linux/notifier.h>

/* Profile mode definitions */
#define KP_MODE_DISABLED	0
#define KP_MODE_BATTERY		1
#define KP_MODE_BALANCED	2
#define KP_MODE_PERFORMANCE	3
#define KP_MODE_MAX		KP_MODE_PERFORMANCE

#ifdef CONFIG_AUTO_KPROFILES_MSM_DRM
#define KP_EVENT_BLANK MSM_DRM_EVENT_BLANK
#define KP_BLANK_POWERDOWN MSM_DRM_BLANK_POWERDOWN
#define KP_BLANK_UNBLANK MSM_DRM_BLANK_UNBLANK
#define kp_events msm_drm_notifier
#elif defined(CONFIG_AUTO_KPROFILES_MI_DRM)
#define KP_EVENT_BLANK MI_DRM_EVENT_BLANK
#define KP_BLANK_POWERDOWN MI_DRM_BLANK_POWERDOWN
#define KP_BLANK_UNBLANK MI_DRM_BLANK_UNBLANK
#define kp_events mi_drm_notifier
#elif defined(CONFIG_AUTO_KPROFILES_FB)
#define KP_EVENT_BLANK FB_EVENT_BLANK
#define KP_BLANK_POWERDOWN FB_BLANK_POWERDOWN
#define KP_BLANK_UNBLANK FB_BLANK_UNBLANK
#define kp_events fb_event
#endif

/* Notifier event for mode changes */
static BLOCKING_NOTIFIER_HEAD(kp_mode_notifier);
unsigned int KP_MODE_CHANGE = 0x80000000;
EXPORT_SYMBOL(KP_MODE_CHANGE);

/* Mode override mechanism */
static unsigned int kp_override_mode;
static bool kp_override;

#ifdef CONFIG_AUTO_KPROFILES
static bool screen_on = true;
#endif

/* Module parameters */
static bool auto_kp __read_mostly = !IS_ENABLED(CONFIG_AUTO_KPROFILES_NONE);
module_param(auto_kp, bool, 0664);
MODULE_PARM_DESC(auto_kp, "Enable/disable automatic kernel profile management");

static unsigned int kp_mode = CONFIG_KP_DEFAULT_MODE;

static struct kobject *kp_kobj;

/* Locking mechanisms */
static DEFINE_MUTEX(kp_set_mode_rb_lock);
static DEFINE_SPINLOCK(kp_set_mode_lock);

/* Debug macros */
#ifdef CONFIG_KP_VERBOSE_DEBUG
#define kp_dbg(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#else
#define kp_dbg(fmt, ...) do { } while (0)
#endif

#define kp_err(fmt, ...) pr_err(fmt, ##__VA_ARGS__)
#define kp_info(fmt, ...) pr_info(fmt, ##__VA_ARGS__)

/* Forward declarations */
static void kp_trigger_mode_change_event(void);
static int kp_validate_mode(unsigned int mode);

/**
 * kp_validate_mode - Validate if a mode is within acceptable range
 * @mode: The mode to validate
 *
 * Return: 0 if valid, -EINVAL otherwise
 */
static inline int kp_validate_mode(unsigned int mode)
{
	if (mode > KP_MODE_MAX)
		return -EINVAL;
	return 0;
}

/**
 * kp_set_mode_rollback - Change profile to a given mode for a specific duration
 * @level: The profile mode level to set (0-3)
 * @duration_ms: The duration to keep the profile mode in milliseconds
 *
 * This function changes the profile to the specified mode for a specific
 * duration during any in-kernel event, and then returns to the previously
 * active mode.
 *
 * Usage example: kp_set_mode_rollback(3, 55);
 */
void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms)
{
#ifdef CONFIG_AUTO_KPROFILES
	if (!screen_on) {
		kp_dbg("Screen is off, skipping mode change\n");
		return;
	}
#endif

	if (!auto_kp) {
		kp_dbg("Auto Kprofiles is disabled, skipping mode change\n");
		return;
	}

	if (kp_validate_mode(level)) {
		kp_err("Invalid mode %u requested, skipping mode change\n", level);
		return;
	}

	mutex_lock(&kp_set_mode_rb_lock);
	
	kp_override_mode = level;
	kp_override = true;
	kp_trigger_mode_change_event();
	
	msleep(duration_ms);
	
	kp_override = false;
	kp_trigger_mode_change_event();
	
	mutex_unlock(&kp_set_mode_rb_lock);
}
EXPORT_SYMBOL(kp_set_mode_rollback);

/**
 * __kp_set_mode - Internal function to set the profile mode
 * @level: The profile mode level to set (0-3)
 *
 * This is an internal helper function. Must be called with appropriate locking.
 *
 * Return: 0 on success, -EINVAL if mode is invalid
 */
static int __kp_set_mode(unsigned int level)
{
	if (kp_validate_mode(level))
		return -EINVAL;

	kp_mode = level;
	kp_dbg("Mode set to %u\n", level);
	
	return 0;
}

/**
 * kp_set_mode - Change profile to a given mode
 * @level: The profile mode level to set (0-3)
 *
 * This function changes the profile to the specified mode during any
 * in-kernel event.
 *
 * Usage example: kp_set_mode(3);
 */
void kp_set_mode(unsigned int level)
{
	unsigned long flags;
	int ret;

#ifdef CONFIG_AUTO_KPROFILES
	if (!screen_on) {
		kp_dbg("Screen is off, skipping mode change\n");
		return;
	}
#endif

	if (!auto_kp) {
		kp_dbg("Auto Kprofiles is disabled, skipping mode change\n");
		return;
	}

	spin_lock_irqsave(&kp_set_mode_lock, flags);
	
	ret = __kp_set_mode(level);
	if (ret) {
		kp_err("Invalid mode %u requested, skipping mode change\n", level);
		spin_unlock_irqrestore(&kp_set_mode_lock, flags);
		return;
	}

	kp_trigger_mode_change_event();
	
	spin_unlock_irqrestore(&kp_set_mode_lock, flags);
}
EXPORT_SYMBOL(kp_set_mode);

/**
 * kp_active_mode - Get the currently active profile mode
 *
 * This function returns a number from 0 to 3 depending on the active profile mode.
 * The returned value can be used in conditions to disable/enable or tune kernel
 * features according to the profile mode.
 *
 * Return:
 * The currently active profile mode (0-3)
 *
 * Usage example:
 *
 * switch (kp_active_mode()) {
 * case KP_MODE_BATTERY:
 *     // Things to be done when battery profile is active
 *     break;
 * case KP_MODE_BALANCED:
 *     // Things to be done when balanced profile is active
 *     break;
 * case KP_MODE_PERFORMANCE:
 *     // Things to be done when performance profile is active
 *     break;
 * default:
 *     // Things to be done when kprofiles is disabled
 *     break;
 * }
 */
int kp_active_mode(void)
{
#ifdef CONFIG_AUTO_KPROFILES
	if (!screen_on && auto_kp)
		return KP_MODE_BATTERY;
#endif

	if (kp_override)
		return kp_override_mode;

	/* Validate and sanitize mode if corrupted */
	if (kp_validate_mode(kp_mode)) {
		kp_err("Invalid mode %u detected, falling back to disabled mode\n", kp_mode);
		kp_mode = KP_MODE_DISABLED;
		kp_trigger_mode_change_event();
	}

	return kp_mode;
}
EXPORT_SYMBOL(kp_active_mode);

/**
 * kp_trigger_mode_change_event - Trigger a mode change event
 *
 * This function triggers a mode change event by calling the blocking notifier
 * chain for kp_mode_notifier. It informs all registered listeners about the
 * change in the profile mode.
 */
static void kp_trigger_mode_change_event(void)
{
	unsigned int current_mode = kp_active_mode();
	
	kp_dbg("Triggering mode change event: mode=%u\n", current_mode);
	
	blocking_notifier_call_chain(&kp_mode_notifier, KP_MODE_CHANGE,
				     (void *)(uintptr_t)current_mode);
}

/**
 * kp_notifier_register_client - Register a notifier client for profile mode changes
 * @nb: The notifier block to register
 *
 * This function registers a notifier client to receive notifications about profile mode changes.
 *
 * Return:
 * 0 on success, or an error code on failure.
 */
int kp_notifier_register_client(struct notifier_block *nb)
{
	if (!nb)
		return -EINVAL;
		
	return blocking_notifier_chain_register(&kp_mode_notifier, nb);
}
EXPORT_SYMBOL(kp_notifier_register_client);

/**
 * kp_notifier_unregister_client - Unregister a notifier client for profile mode changes
 * @nb: The notifier block to unregister
 *
 * This function unregisters a previously registered notifier client for profile mode changes.
 *
 * Return:
 * 0 on success, or an error code on failure.
 */
int kp_notifier_unregister_client(struct notifier_block *nb)
{
	if (!nb)
		return -EINVAL;
		
	return blocking_notifier_chain_unregister(&kp_mode_notifier, nb);
}
EXPORT_SYMBOL(kp_notifier_unregister_client);

#ifdef CONFIG_AUTO_KPROFILES
/**
 * kp_display_notifier_callback - Callback for display state changes
 * @self: The notifier block
 * @event: The event type
 * @data: Event-specific data
 *
 * This callback is invoked when the display state changes (screen on/off).
 * It automatically switches to battery mode when the screen turns off and
 * restores the previous mode when the screen turns back on.
 *
 * Return: NOTIFY_OK
 */
static int kp_display_notifier_callback(struct notifier_block *self,
					unsigned long event, void *data)
{
	struct kp_events *evdata = data;
	unsigned int blank;

	if (event != KP_EVENT_BLANK)
		return NOTIFY_OK;

	if (!evdata || !evdata->data)
		return NOTIFY_OK;

	blank = *(int *)(evdata->data);
	
	switch (blank) {
	case KP_BLANK_POWERDOWN:
		if (screen_on) {
			screen_on = false;
			kp_dbg("Screen turned off, switching to battery mode\n");
			kp_trigger_mode_change_event();
		}
		break;
		
	case KP_BLANK_UNBLANK:
		if (!screen_on) {
			screen_on = true;
			kp_dbg("Screen turned on, restoring previous mode\n");
			kp_trigger_mode_change_event();
		}
		break;
		
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block kp_display_notifier_block = {
	.notifier_call = kp_display_notifier_callback,
};

/**
 * kp_register_display_notifier - Register display state notifier
 *
 * Return: 0 on success, error code otherwise
 */
static int kp_register_display_notifier(void)
{
	int ret = 0;

#ifdef CONFIG_AUTO_KPROFILES_MSM_DRM
	ret = msm_drm_register_client(&kp_display_notifier_block);
	if (ret)
		kp_err("Failed to register MSM DRM notifier: %d\n", ret);
#elif defined(CONFIG_AUTO_KPROFILES_MI_DRM)
	ret = mi_drm_register_client(&kp_display_notifier_block);
	if (ret)
		kp_err("Failed to register MI DRM notifier: %d\n", ret);
#elif defined(CONFIG_AUTO_KPROFILES_FB)
	ret = fb_register_client(&kp_display_notifier_block);
	if (ret)
		kp_err("Failed to register FB notifier: %d\n", ret);
#endif

	return ret;
}

/**
 * kp_unregister_display_notifier - Unregister display state notifier
 */
static void kp_unregister_display_notifier(void)
{
#ifdef CONFIG_AUTO_KPROFILES_MSM_DRM
	msm_drm_unregister_client(&kp_display_notifier_block);
#elif defined(CONFIG_AUTO_KPROFILES_MI_DRM)
	mi_drm_unregister_client(&kp_display_notifier_block);
#elif defined(CONFIG_AUTO_KPROFILES_FB)
	fb_unregister_client(&kp_display_notifier_block);
#endif
}

#else /* !CONFIG_AUTO_KPROFILES */

static inline int kp_register_display_notifier(void)
{
	return 0;
}

static inline void kp_unregister_display_notifier(void)
{
}

#endif /* CONFIG_AUTO_KPROFILES */

/* Sysfs interface */

/**
 * kp_mode_show - Show current profile mode via sysfs
 */
static ssize_t kp_mode_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", kp_mode);
}

/**
 * kp_mode_store - Store new profile mode via sysfs
 */
static ssize_t kp_mode_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int new_mode;
	int ret;

	ret = kstrtouint(buf, 10, &new_mode);
	if (ret) {
		kp_err("Invalid input: %s\n", buf);
		return ret;
	}

	ret = __kp_set_mode(new_mode);
	if (ret) {
		kp_err("Invalid mode %u, must be 0-%u\n", new_mode, KP_MODE_MAX);
		return ret;
	}

	kp_trigger_mode_change_event();
	kp_info("Mode changed to %u via sysfs\n", new_mode);

	return count;
}

/**
 * auto_kp_show - Show auto_kp status via sysfs
 */
static ssize_t auto_kp_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", auto_kp ? 1 : 0);
}

/**
 * auto_kp_store - Store auto_kp status via sysfs
 */
static ssize_t auto_kp_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool new_state;
	int ret;

	ret = kstrtobool(buf, &new_state);
	if (ret)
		return ret;

	auto_kp = new_state;
	kp_info("Auto Kprofiles %s\n", auto_kp ? "enabled" : "disabled");

	return count;
}

static struct kobj_attribute kp_mode_attribute =
	__ATTR(kp_mode, 0664, kp_mode_show, kp_mode_store);

static struct kobj_attribute auto_kp_attribute =
	__ATTR(auto_kp, 0664, auto_kp_show, auto_kp_store);

static struct attribute *kp_attrs[] = {
	&kp_mode_attribute.attr,
	&auto_kp_attribute.attr,
	NULL,
};

static struct attribute_group kp_attr_group = {
	.attrs = kp_attrs,
};

/* Module initialization and cleanup */

static int __init kp_init(void)
{
	int ret;

	/* Validate default mode */
	if (kp_validate_mode(CONFIG_KP_DEFAULT_MODE)) {
		kp_err("Invalid default mode %u, using mode 0\n", CONFIG_KP_DEFAULT_MODE);
		kp_mode = KP_MODE_DISABLED;
	}

	/* Create kobject */
	kp_kobj = kobject_create_and_add("kprofiles", kernel_kobj);
	if (!kp_kobj) {
		kp_err("Failed to create Kprofiles kobject\n");
		return -ENOMEM;
	}

	/* Create sysfs attributes */
	ret = sysfs_create_group(kp_kobj, &kp_attr_group);
	if (ret) {
		kp_err("Failed to create sysfs attributes: %d\n", ret);
		goto err_sysfs;
	}

	/* Register display notifier if auto mode is enabled */
	ret = kp_register_display_notifier();
	if (ret) {
		kp_err("Failed to register display notifier: %d\n", ret);
		goto err_notifier;
	}

	kp_info("Kprofiles " KPROFILES_VERSION " initialized successfully\n");
	kp_info("Default mode: %u, Auto Kprofiles: %s\n", 
		kp_mode, auto_kp ? "enabled" : "disabled");
	kp_info("For documentation, visit: https://github.com/beakthoven/Kprofiles\n");

	return 0;

err_notifier:
	sysfs_remove_group(kp_kobj, &kp_attr_group);
err_sysfs:
	kobject_put(kp_kobj);
	return ret;
}
module_init(kp_init);

static void __exit kp_exit(void)
{
	kp_unregister_display_notifier();
	sysfs_remove_group(kp_kobj, &kp_attr_group);
	kobject_put(kp_kobj);
	
	kp_info("Kprofiles unloaded\n");
}
module_exit(kp_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("KernelSpace Profiles");
MODULE_AUTHOR("Dakkshesh <dakkshesh5@gmail.com>");
MODULE_VERSION(KPROFILES_VERSION);
