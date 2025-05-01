load(
    "//build/kernel/kleaf:kernel.bzl",
    "ddk_module",
    "kernel_modules_install",
)
load(
    ":platform_modules.bzl",
    "platform_modules",
    "platform_modules_by_config",
)
load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")

def _replace_formatting_codes(target, variant, s):
    kernel_build = "{}_{}".format(target, variant)

    return s.replace("%b", kernel_build).replace("%t", target)

def _console_print(target, variant, module, message):
    if module:
        print("{}: {}: platform-kernel: {}: {}".format(target, variant, module, message))
    else:
        print("{}: {}: platform-kernel: {} ".format(target, variant, message))

def _get_options(target, variant, target_config_option, modules, extra_options):
    all_options = {option: True for option in extra_options}

    redundant_options = []

    for option in platform_modules_by_config:
        module_name = platform_modules_by_config[option]

        if option in all_options:
            if module_name in modules:
                redundant_options.append(option)
            else:
                _console_print(target, variant, None, 'WARNING: Config option "{}" corresponds to platform module {}, but this module is not listed in module list!'.format(option, module_name))
        else:
            all_options[option] = True

    if target_config_option in all_options:
        redundant_options.append(target_config_option)
    else:
        all_options[target_config_option] = True

    if redundant_options:
        _console_print(target, variant, None, "INFO: The following options are already declared either by a module or the target, no need to redeclare: \n{}".format("\n".join(redundant_options)))

    return all_options

def _get_module_srcs(target, variant, module, options):
    srcs = [] + module["default_srcs"] + module["srcs"]
    module_path = "{}/".format(module["path"]) if module["path"] else ""
    for option in module["config_srcs"]:
        srcs.extend(module["config_srcs"][option].get(option in options, []))

    globbed_srcs = native.glob(["{}{}".format(module_path, _replace_formatting_codes(target, variant, src)) for src in srcs])

    if not globbed_srcs:
        _console_print(target, variant, module["name"], "WARNING: Module has no sources attached!")

    return globbed_srcs

def define_target_variant_modules(target, variant, modules, extra_options = [], config_option = None):
    kernel_build_variant = "{}_{}".format(target, variant)
    options = _get_options(target, variant, config_option, modules, extra_options)
    module_rules = []
    target_local_defines = []
    modules = [platform_modules[module_name] for module_name in modules]
    tv = "{}_{}".format(target, variant)
    kernel_build = select({
        "//build/kernel/kleaf:socrepo_true": "//soc-repo:{}_base_kernel".format(tv),
        "//build/kernel/kleaf:socrepo_false": "//msm-kernel:{}".format(tv),
    })
    deps = select({
        "//build/kernel/kleaf:socrepo_true": [
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/remoteproc/rproc_qcom_common".format(kernel_build_variant),
        ],
	"//build/kernel/kleaf:socrepo_false": ["//msm-kernel:all_headers"],
    })

    for config in extra_options:
        target_local_defines.append(config)
    for module in modules:
        rule_name = "{}_{}".format(kernel_build_variant, module["name"])
        module_srcs = _get_module_srcs(target, variant, module, options)

        ddk_module(
            name = rule_name,
	    kernel_build = kernel_build,
            srcs = module_srcs,
            out = "{}.ko".format(module["name"]),
            deps = deps + [_replace_formatting_codes(target, variant, dep) for dep in module["deps"]],
            hdrs = module["hdrs"],
            local_defines = target_local_defines,
            copts = module["copts"],
        )
        module_rules.append(rule_name)

    copy_to_dist_dir(
        name = "{}_platform-kernel_dist".format(kernel_build_variant),
        data = module_rules,
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )

    kernel_modules_install(
        name = "{}_modules_install".format(kernel_build_variant),
        kernel_build = kernel_build,
        kernel_modules = module_rules,
    )

def define_consolidate_gki_perf_modules(target, modules, extra_options = [], config_option = None):
    define_target_variant_modules(target, "consolidate", modules, extra_options, config_option)
    define_target_variant_modules(target, "gki", modules, extra_options, config_option)
    define_target_variant_modules(target, "perf", modules, extra_options, config_option)
