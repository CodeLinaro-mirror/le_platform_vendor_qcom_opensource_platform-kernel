load(":platform_kernel.bzl", "define_consolidate_gki_perf_modules")

def define_autogvm():
    define_consolidate_gki_perf_modules(
        target = "autogvm",
        modules = [
	    "wallpower_charger",
	    "boot_marker",
	    "socinfo_dt",
	    "vm-cpufreq",
	    "virtio_ssr",
	    "compressched_fe",
	    "subsystem_notif_virt",
	    "hfastrpc"
	],
    )
