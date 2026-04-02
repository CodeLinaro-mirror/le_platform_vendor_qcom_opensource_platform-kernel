DRIVERS_PATH = "drivers"
DSP_PATH = "drivers/virtual_fastrpc/dsp"
COMPRESSCHED_FE_PATH = "drivers/compressched_fe"
platform_modules = {}
platform_modules_by_config = {}

# Registers platform module to kernel build system.
# name: The name of the module. The name of the file generated for this module will be {name}.ko.
# path: The path that will be prepended to all sources listed for this module.
# config_option: If this module is enabled, the config optiont that will get enabled if so. Not all modules have this, and this is an optional parameter.
# config_srcs: A dictionary of sources to be added to the module depending on if a configuration option is enabled or not. The keys to the dictionary are
# the name of the config option, and the value depends If it is a list, it will just be the list of sources to be added to the module if the config option
# is enabled. If the value is another dictionary, then you can specify sources to be added if the config option is DISABLED by having a list under the
# default_srcs: A list of sources to be added to the module regardless of configuration options.
# deps: A list of kernel_module or ddk_module rules that this module depends on.
# config_deps: A dictionary of kernel_module or ddk_module rules that this module depends on if a configuration option is enabled or not. The keys to the
# dictionary are the name of the config option, and the value depends If it is a list, it will just be the list of kernel_module or ddk_module rules to
# be added to the module if the config option is enabled.
# config_copts: A dictionary of compile options used for building this target if a configuration option is enabled or not. The keys to the dictionary are
# the name of the config option, and the value depends If it is a list, it will just be the compile options used for building this target to
# be added to the module if the config option is enabled.

def register_platform_kernel_module(name, path = None, config_option = None, default_srcs = [], config_srcs = {}, deps = [], config_deps = {}, srcs = [], copts = [], config_copts = {}, hdrs = []):
    processed_config_srcs = {}
    for config_src_name in config_srcs:
        config_src = config_srcs[config_src_name]

        if type(config_src) == "list":
            processed_config_srcs[config_src_name] = {True: config_src}
        else:
            processed_config_srcs[config_src_name] = config_src

    processed_config_deps = {}
    for config_dep_name in config_deps:
        config_dep = config_deps[config_dep_name]

        if type(config_dep) == "list":
            processed_config_deps[config_dep_name] = {True: config_dep}
        else:
            processed_config_deps[config_dep_name] = config_dep

    processed_config_copts = {}
    for config_copt_name in config_copts:
        config_copt = config_copts[config_copt_name]

        if type(config_copt) == "list":
            processed_config_copts[config_copt_name] = {True: config_copt}
        else:
            processed_config_copts[config_copt_name] = config_copt

    module = {
        "name": name,
        "path": path,
        "default_srcs": default_srcs,
        "config_srcs": processed_config_srcs,
        "config_option": config_option,
        "deps": deps,
        "config_deps": processed_config_deps,
        "copts": copts,
        "config_copts": processed_config_copts,
        "srcs": srcs,
        "hdrs": hdrs,
    }

    platform_modules[name] = module

    if config_option:
        platform_modules_by_config[config_option] = name

# ------------------------------------ PLATFORM MODULE DEFINITIONS ---------------------------------
register_platform_kernel_module(
    name = "aop-set-ddr",
    path = DRIVERS_PATH,
    default_srcs = ["aop-set-ddr.c"],
)

register_platform_kernel_module(
    name = "boot_marker",
    path = DRIVERS_PATH,
    deps = [":boot_marker_headers"],
    default_srcs = ["boot_marker.c"],
)

register_platform_kernel_module(
    name = "wallpower_charger",
    path = DRIVERS_PATH,
    default_srcs = ["wallpower_charger.c"],
)

register_platform_kernel_module(
    name = "silent_boot",
    path = DRIVERS_PATH,
    default_srcs = ["silent_boot.c"],
)

register_platform_kernel_module(
    name = "silent-mode-hw-monitoring",
    path = DRIVERS_PATH,
    default_srcs = ["silent-mode-hw-monitoring.c"],
    deps = ["%b_silent_boot"],
)

register_platform_kernel_module(
    name = "dump_boot_log",
    path = DRIVERS_PATH,
    default_srcs = ["dump_boot_log.c"],
)

register_platform_kernel_module(
    name = "s2r_wakeup_marker",
    path = DRIVERS_PATH,
    default_srcs = ["s2r_wakeup_marker.c"],
)

register_platform_kernel_module(
    name = "mem-online",
    path = DRIVERS_PATH,
    default_srcs = ["mem-online.c"],
)

register_platform_kernel_module(
    name = "subsystem_status",
    path = DRIVERS_PATH,
    default_srcs = ["subsystem_status.c"],
)

register_platform_kernel_module(
    name = "socinfo_dt",
    path = DRIVERS_PATH,
    default_srcs = ["socinfo_dt.c"],
)

register_platform_kernel_module(
    name = "adsp_vote_smp2p",
    path = DRIVERS_PATH,
    default_srcs = ["adsp_vote_smp2p.c"],
)

register_platform_kernel_module(
    name = "vm-cpufreq",
    path = DRIVERS_PATH,
    default_srcs = ["vm-cpufreq.c"],
)

register_platform_kernel_module(
    name = "subsystem_notif_virt",
    path = DRIVERS_PATH,
    default_srcs = [
	"subsystem_notif_virt.c",
	"qcom_common.h",
    ],
)

register_platform_kernel_module(
    name = "virtio_ssr",
    path = DRIVERS_PATH,
    default_srcs = [
        "virtio_ssr.c",
	"qcom_common.h",
    ],
)

register_platform_kernel_module(
    name = "compressched_fe",
    path = COMPRESSCHED_FE_PATH,
    config_option = "CONFIG_HYBRID_FASTRPC_COMPRESSCHED",
    default_srcs = [
        "virtio_compressched_base.c",
        "virtio_compressched_base.h",
        "virtio_compressched_client.c",
        "virtio_compressched_client.h"
    ],
)

# For the RSM function to work, fastRPC BE configuration is still required
register_platform_kernel_module(
    name = "hfastrpc",
    path = DSP_PATH,
    default_srcs = [
        "hybrid_fastrpc_vdev.c",
        "fastrpc_core.c",
        "fastrpc_rpmsg.c",
        "fastrpc_ioctl.c",
        "fastrpc_mem.c",
        "fastrpc_vq.c",
        "fastrpc_sysfs.c",
        "fastrpc_common.h",
        "fastrpc_mem.h",
        "fastrpc_core.h",
        "fastrpc_vq.h",
    ],
    config_srcs = {
        "CONFIG_HYBRID_FASTRPC_COMPRESSCHED": [
            "fastrpc_rsm.c",
            "fastrpc_rsm.h",
         ],
    },
    deps = [":fastrpc_local_headers"],
    config_deps = {
        "CONFIG_HYBRID_FASTRPC_COMPRESSCHED": [":%b_compressched_fe"],
    },
    config_copts = {
        "CONFIG_HYBRID_FASTRPC_COMPRESSCHED": ["-DCONFIG_HYBRID_FASTRPC_RSM=1"],
    },
)
