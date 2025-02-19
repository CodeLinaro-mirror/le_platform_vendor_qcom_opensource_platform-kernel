load(":platform_kernel.bzl", "define_consolidate_gki_perf_modules")

def define_sdmsteppeauto():
    define_consolidate_gki_perf_modules(
        target = "sdmsteppeauto",
        modules = [
	    "aop-set-ddr",
	    "boot_marker",
	    "wallpower_charger",
	    "silent_boot",
	    "silent-mode-hw-monitoring",
	    "dump_boot_log",
	    "s2r_wakeup_marker",
	    "mem-online",
	    "subsystem_status",
	    "adsp_vote_smp2p"
        ],
        extra_options = [
        ],
    )
