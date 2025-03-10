#!/usr/bin/env python3

# Copyright 2020 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Runs the local presubmit checks for the Pigweed repository."""

import argparse
import json
import logging
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
from typing import Callable, Iterable, Sequence, TextIO

from pw_cli.plural import plural
from pw_cli.file_filter import FileFilter
import pw_package.pigweed_packages
from pw_presubmit import (
    build,
    cli,
    cpp_checks,
    format_code,
    git_repo,
    gitmodules,
    inclusive_language,
    javascript_checks,
    json_check,
    keep_sorted,
    module_owners,
    npm_presubmit,
    owners_checks,
    python_checks,
    shell_checks,
    source_in_build,
    todo_check,
)
from pw_presubmit.presubmit import (
    Programs,
    call,
    filter_paths,
)
from pw_presubmit.presubmit_context import (
    PresubmitContext,
    PresubmitFailure,
)
from pw_presubmit.tools import log_run
from pw_presubmit.install_hook import install_git_hook

_LOG = logging.getLogger(__name__)

pw_package.pigweed_packages.initialize()

# Trigger builds if files with these extensions change.
_BUILD_FILE_FILTER = FileFilter(
    suffix=(
        *format_code.C_FORMAT.extensions,
        '.cfg',
        '.py',
        '.rst',
        '.gn',
        '.gni',
        '.emb',
    )
)

_OPTIMIZATION_LEVELS = 'debug', 'size_optimized', 'speed_optimized'


def _at_all_optimization_levels(target):
    for level in _OPTIMIZATION_LEVELS:
        yield f'{target}_{level}'


class PigweedGnGenNinja(build.GnGenNinja):
    """Add Pigweed-specific defaults to GnGenNinja."""

    def add_default_gn_args(self, args):
        """Add project-specific default GN args to 'args'."""
        args['pw_C_OPTIMIZATION_LEVELS'] = ('debug',)


def build_bazel(*args, **kwargs) -> None:
    build.bazel(*args, use_remote_cache=True, **kwargs)


#
# Build presubmit checks
#
gn_all = PigweedGnGenNinja(
    name='gn_all',
    path_filter=_BUILD_FILE_FILTER,
    gn_args=dict(pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS),
    ninja_targets=('all',),
)


def gn_clang_build(ctx: PresubmitContext):
    """Checks all compile targets that rely on LLVM tooling."""
    build_targets = [
        *_at_all_optimization_levels('host_clang'),
        'cpp20_compatibility',
        'asan',
        'tsan',
        'ubsan',
        'runtime_sanitizers',
        # TODO: b/234876100 - msan will not work until the C++ standard library
        # included in the sysroot has a variant built with msan.
    ]

    # clang-tidy doesn't run on Windows.
    if sys.platform != 'win32':
        build_targets.append('static_analysis')

    # QEMU doesn't run on Windows.
    if sys.platform != 'win32':
        # TODO: b/244604080 - For the pw::InlineString tests, qemu_clang_debug
        #     and qemu_clang_speed_optimized produce a binary too large for the
        #     QEMU target's 256KB flash. Restore debug and speed optimized
        #     builds when this is fixed.
        build_targets.append('qemu_clang_size_optimized')

    # TODO: b/240982565 - SocketStream currently requires Linux.
    if sys.platform.startswith('linux'):
        build_targets.append('integration_tests')

    build.gn_gen(ctx, pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS)
    build.ninja(ctx, *build_targets)
    build.gn_check(ctx)


_HOST_COMPILER = 'gcc' if sys.platform == 'win32' else 'clang'


@filter_paths(file_filter=_BUILD_FILE_FILTER)
def gn_quick_build_check(ctx: PresubmitContext):
    """Checks the state of the GN build by running gn gen and gn check."""
    build.gn_gen(ctx)


def _gn_combined_build_check_targets() -> Sequence[str]:
    build_targets = [
        'check_modules',
        *_at_all_optimization_levels('stm32f429i'),
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        'python.tests',
        'python.lint',
        'pigweed_pypi_distribution',
    ]

    # TODO: b/315998985 - Add docs back to Mac ARM build.
    if sys.platform != 'darwin' or platform.machine() != 'arm64':
        build_targets.append('docs')

    # C headers seem to be missing when building with pw_minimal_cpp_stdlib, so
    # skip it on Windows.
    if sys.platform != 'win32':
        build_targets.append('build_with_pw_minimal_cpp_stdlib')

    # TODO: b/234645359 - Re-enable on Windows when compatibility tests build.
    if sys.platform != 'win32':
        build_targets.append('cpp20_compatibility')

    # clang-tidy doesn't run on Windows.
    if sys.platform != 'win32':
        build_targets.append('static_analysis')

    # QEMU doesn't run on Windows.
    if sys.platform != 'win32':
        # TODO: b/244604080 - For the pw::InlineString tests, qemu_*_debug
        #     and qemu_*_speed_optimized produce a binary too large for the
        #     QEMU target's 256KB flash. Restore debug and speed optimized
        #     builds when this is fixed.
        build_targets.append('qemu_gcc_size_optimized')
        build_targets.append('qemu_clang_size_optimized')

    # TODO: b/240982565 - SocketStream currently requires Linux.
    if sys.platform.startswith('linux'):
        build_targets.append('integration_tests')

    # TODO: b/269354373 - clang is not supported on windows yet
    if sys.platform != 'win32':
        build_targets.append('host_clang_debug_dynamic_allocation')

    return build_targets


gn_combined_build_check = PigweedGnGenNinja(
    name='gn_combined_build_check',
    doc='Run most host and device (QEMU) tests.',
    path_filter=_BUILD_FILE_FILTER,
    gn_args=dict(
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
        pw_BUILD_BROKEN_GROUPS=True,  # Enable to fully test the GN build
    ),
    ninja_targets=_gn_combined_build_check_targets(),
)

coverage = PigweedGnGenNinja(
    name='coverage',
    doc='Run coverage for the host build.',
    path_filter=_BUILD_FILE_FILTER,
    ninja_targets=('coverage',),
    coverage_options=build.CoverageOptions(
        common=build.CommonCoverageOptions(
            target_bucket_project='pigweed',
            target_bucket_root='gs://ng3-metrics/ng3-pigweed-coverage',
            trace_type='LLVM',
            owner='pigweed-infra@google.com',
            bug_component='503634',
        ),
        codesearch=(
            build.CodeSearchCoverageOptions(
                host='pigweed-internal',
                project='codesearch',
                add_prefix='pigweed',
                ref='refs/heads/main',
                source='infra:main',
            ),
        ),
        gerrit=build.GerritCoverageOptions(
            project='pigweed/pigweed',
        ),
    ),
)


@filter_paths(file_filter=_BUILD_FILE_FILTER)
def gn_arm_build(ctx: PresubmitContext):
    build.gn_gen(ctx, pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS)
    build.ninja(ctx, *_at_all_optimization_levels('stm32f429i'))
    build.gn_check(ctx)


stm32f429i = PigweedGnGenNinja(
    name='stm32f429i',
    path_filter=_BUILD_FILE_FILTER,
    gn_args={
        'pw_use_test_server': True,
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_contexts=(
        lambda ctx: build.test_server(
            'stm32f429i_disc1_test_server',
            ctx.output_dir,
        ),
    ),
    ninja_targets=_at_all_optimization_levels('stm32f429i'),
)

gn_crypto_mbedtls_build = PigweedGnGenNinja(
    name='gn_crypto_mbedtls_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('mbedtls',),
    gn_args={
        'dir_pw_third_party_mbedtls': lambda ctx: '"{}"'.format(
            ctx.package_root / 'mbedtls'
        ),
        'pw_crypto_SHA256_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:sha256_mbedtls_v3'
        ),
        'pw_crypto_ECDSA_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdsa_mbedtls_v3'
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=(
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        # TODO: b/240982565 - SocketStream currently requires Linux.
        *(('integration_tests',) if sys.platform.startswith('linux') else ()),
    ),
)

gn_crypto_micro_ecc_build = PigweedGnGenNinja(
    name='gn_crypto_micro_ecc_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('micro-ecc',),
    gn_args={
        'dir_pw_third_party_micro_ecc': lambda ctx: '"{}"'.format(
            ctx.package_root / 'micro-ecc'
        ),
        'pw_crypto_ECDSA_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdsa_uecc'
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=(
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        # TODO: b/240982565 - SocketStream currently requires Linux.
        *(('integration_tests',) if sys.platform.startswith('linux') else ()),
    ),
)

gn_teensy_build = PigweedGnGenNinja(
    name='gn_teensy_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('teensy',),
    gn_args={
        'pw_arduino_build_CORE_PATH': lambda ctx: '"{}"'.format(
            str(ctx.package_root)
        ),
        'pw_arduino_build_CORE_NAME': 'teensy',
        'pw_arduino_build_PACKAGE_NAME': 'avr/1.58.1',
        'pw_arduino_build_BOARD': 'teensy40',
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=_at_all_optimization_levels('arduino'),
)

gn_pico_build = PigweedGnGenNinja(
    name='gn_pico_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('pico_sdk', 'freertos'),
    gn_args={
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'freertos')
        ),
        'PICO_SRC_DIR': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'pico_sdk')
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('pi_pico',),
)

gn_mimxrt595_build = PigweedGnGenNinja(
    name='gn_mimxrt595_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('mcuxpresso',),
    gn_args={
        'dir_pw_third_party_mcuxpresso': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'mcuxpresso')
        ),
        'pw_target_mimxrt595_evk_MANIFEST': '$dir_pw_third_party_mcuxpresso'
        + '/EVK-MIMXRT595_manifest_v3_13.xml',
        'pw_third_party_mcuxpresso_SDK': '//targets/mimxrt595_evk:sample_sdk',
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('mimxrt595'),
)

gn_mimxrt595_freertos_build = PigweedGnGenNinja(
    name='gn_mimxrt595_freertos_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('freertos', 'mcuxpresso'),
    gn_args={
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'freertos')
        ),
        'dir_pw_third_party_mcuxpresso': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'mcuxpresso')
        ),
        'pw_target_mimxrt595_evk_freertos_MANIFEST': '{}/{}'.format(
            "$dir_pw_third_party_mcuxpresso", "EVK-MIMXRT595_manifest_v3_13.xml"
        ),
        'pw_third_party_mcuxpresso_SDK': '//targets/mimxrt595_evk_freertos:sdk',
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=('mimxrt595_freertos'),
)

gn_software_update_build = PigweedGnGenNinja(
    name='gn_software_update_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('nanopb', 'protobuf', 'mbedtls', 'micro-ecc'),
    gn_args={
        'dir_pw_third_party_protobuf': lambda ctx: '"{}"'.format(
            ctx.package_root / 'protobuf'
        ),
        'dir_pw_third_party_nanopb': lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        'dir_pw_third_party_micro_ecc': lambda ctx: '"{}"'.format(
            ctx.package_root / 'micro-ecc'
        ),
        'pw_crypto_ECDSA_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:ecdsa_uecc'
        ),
        'dir_pw_third_party_mbedtls': lambda ctx: '"{}"'.format(
            ctx.package_root / 'mbedtls'
        ),
        'pw_crypto_SHA256_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_crypto:sha256_mbedtls_v3'
        ),
        'pw_C_OPTIMIZATION_LEVELS': _OPTIMIZATION_LEVELS,
    },
    ninja_targets=_at_all_optimization_levels('host_clang'),
)

gn_pw_system_demo_build = PigweedGnGenNinja(
    name='gn_pw_system_demo_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('freertos', 'nanopb', 'stm32cube_f4', 'pico_sdk'),
    gn_args={
        'dir_pw_third_party_freertos': lambda ctx: '"{}"'.format(
            ctx.package_root / 'freertos'
        ),
        'dir_pw_third_party_nanopb': lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        'dir_pw_third_party_stm32cube_f4': lambda ctx: '"{}"'.format(
            ctx.package_root / 'stm32cube_f4'
        ),
        'PICO_SRC_DIR': lambda ctx: '"{}"'.format(
            str(ctx.package_root / 'pico_sdk')
        ),
    },
    ninja_targets=('pw_system_demo',),
)

gn_chre_googletest_nanopb_sapphire_build = PigweedGnGenNinja(
    name='gn_chre_googletest_nanopb_sapphire_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('boringssl', 'chre', 'emboss', 'googletest', 'icu', 'nanopb'),
    gn_args=dict(
        dir_pw_third_party_chre=lambda ctx: '"{}"'.format(
            ctx.package_root / 'chre'
        ),
        dir_pw_third_party_nanopb=lambda ctx: '"{}"'.format(
            ctx.package_root / 'nanopb'
        ),
        dir_pw_third_party_googletest=lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        dir_pw_third_party_emboss=lambda ctx: '"{}"'.format(
            ctx.package_root / 'emboss'
        ),
        dir_pw_third_party_boringssl=lambda ctx: '"{}"'.format(
            ctx.package_root / 'boringssl'
        ),
        dir_pw_third_party_icu=lambda ctx: '"{}"'.format(
            ctx.package_root / 'icu'
        ),
        pw_unit_test_MAIN=lambda ctx: '"{}"'.format(
            ctx.root / 'third_party/googletest:gmock_main'
        ),
        pw_unit_test_BACKEND=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_unit_test:googletest'
        ),
        pw_function_CONFIG=lambda ctx: '"{}"'.format(
            ctx.root / 'pw_function:enable_dynamic_allocation'
        ),
        pw_bluetooth_sapphire_ENABLED=True,
        pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS,
    ),
    ninja_targets=(
        *_at_all_optimization_levels(f'host_{_HOST_COMPILER}'),
        *_at_all_optimization_levels('stm32f429i'),
    ),
)

gn_fuzz_build = PigweedGnGenNinja(
    name='gn_fuzz_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('abseil-cpp', 'fuzztest', 'googletest', 're2'),
    gn_args={
        'dir_pw_third_party_abseil_cpp': lambda ctx: '"{}"'.format(
            ctx.package_root / 'abseil-cpp'
        ),
        'dir_pw_third_party_fuzztest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'fuzztest'
        ),
        'dir_pw_third_party_googletest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        'dir_pw_third_party_re2': lambda ctx: '"{}"'.format(
            ctx.package_root / 're2'
        ),
        'pw_unit_test_MAIN': lambda ctx: '"{}"'.format(
            ctx.root / 'third_party/googletest:gmock_main'
        ),
        'pw_unit_test_BACKEND': lambda ctx: '"{}"'.format(
            ctx.root / 'pw_unit_test:googletest'
        ),
    },
    ninja_targets=('fuzzers',),
    ninja_contexts=(
        lambda ctx: build.modified_env(
            FUZZTEST_PRNG_SEED=build.fuzztest_prng_seed(ctx),
        ),
    ),
)

oss_fuzz_build = PigweedGnGenNinja(
    name='oss_fuzz_build',
    path_filter=_BUILD_FILE_FILTER,
    packages=('abseil-cpp', 'fuzztest', 'googletest', 're2'),
    gn_args={
        'dir_pw_third_party_abseil_cpp': lambda ctx: '"{}"'.format(
            ctx.package_root / 'abseil-cpp'
        ),
        'dir_pw_third_party_fuzztest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'fuzztest'
        ),
        'dir_pw_third_party_googletest': lambda ctx: '"{}"'.format(
            ctx.package_root / 'googletest'
        ),
        'dir_pw_third_party_re2': lambda ctx: '"{}"'.format(
            ctx.package_root / 're2'
        ),
        'pw_toolchain_OSS_FUZZ_ENABLED': True,
    },
    ninja_targets=('oss_fuzz',),
)


def _env_with_zephyr_vars(ctx: PresubmitContext) -> dict:
    """Returns the environment variables with ... set for Zephyr."""
    env = os.environ.copy()
    # Set some variables here.
    env['ZEPHYR_BASE'] = str(ctx.package_root / 'zephyr')
    env['ZEPHYR_MODULES'] = str(ctx.root)
    env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'llvm'
    return env


def zephyr_build(ctx: PresubmitContext) -> None:
    """Run Zephyr compatible tests"""
    # Install the Zephyr package
    build.install_package(ctx, 'zephyr')
    # Configure the environment
    env = _env_with_zephyr_vars(ctx)
    # Get the python twister runner
    twister = ctx.package_root / 'zephyr' / 'scripts' / 'twister'
    # Get a list of the test roots
    testsuite_roots = [
        ctx.pw_root / dir
        for dir in os.listdir(ctx.pw_root)
        if dir.startswith('pw_')
    ]
    testsuite_roots_list = [
        args for dir in testsuite_roots for args in ('--testsuite-root', dir)
    ]
    sysroot_dir = (
        ctx.pw_root
        / 'environment'
        / 'cipd'
        / 'packages'
        / 'pigweed'
        / 'clang_sysroot'
    )
    platform_filters = (
        ['-P', 'native_posix', '-P', 'native_sim']
        if platform.system() in ['Windows', 'Darwin']
        else []
    )
    # Run twister
    call(
        sys.executable,
        twister,
        '--ninja',
        '--integration',
        '--clobber-output',
        '--inline-logs',
        '--verbose',
        *platform_filters,
        '-x=CONFIG_LLVM_USE_LLD=y',
        '-x=CONFIG_COMPILER_RT_RTLIB=y',
        f'-x=TOOLCHAIN_C_FLAGS=--sysroot={sysroot_dir}',
        f'-x=TOOLCHAIN_LD_FLAGS=--sysroot={sysroot_dir}',
        *testsuite_roots_list,
        env=env,
    )
    # Produces reports at (ctx.root / 'twister_out' / 'twister*.xml')


def docs_build(ctx: PresubmitContext) -> None:
    """Build Pigweed docs"""

    build.install_package(ctx, 'emboss')
    build.install_package(ctx, 'freertos')
    build.install_package(ctx, 'nanopb')
    build.install_package(ctx, 'pico_sdk')
    build.install_package(ctx, 'pigweed_examples_repo')
    build.install_package(ctx, 'stm32cube_f4')

    # Build main docs through GN/Ninja.
    build.gn_gen(ctx, pw_C_OPTIMIZATION_LEVELS=_OPTIMIZATION_LEVELS)
    build.ninja(ctx, 'docs')
    build.gn_check(ctx)

    # Build Rust docs through Bazel.
    build_bazel(
        ctx,
        'build',
        '--',
        '//pw_rust:docs',
    )

    # Build examples repo docs through GN.
    examples_repo_root = ctx.package_root / 'pigweed_examples_repo'
    examples_repo_out = examples_repo_root / 'out'

    # Setup an examples repo presubmit context.
    examples_ctx = PresubmitContext(
        root=examples_repo_root,
        repos=(examples_repo_root,),
        output_dir=examples_repo_out,
        failure_summary_log=ctx.failure_summary_log,
        paths=tuple(),
        all_paths=tuple(),
        package_root=ctx.package_root,
        luci=None,
        override_gn_args={},
        num_jobs=ctx.num_jobs,
        continue_after_build_error=True,
        _failed=False,
        format_options=ctx.format_options,
    )

    # Write a pigweed_environment.gni for the examples repo.
    pwenvgni = (
        ctx.root / 'build_overrides/pigweed_environment.gni'
    ).read_text()
    # Fix the path for cipd packages.
    pwenvgni.replace('../environment/cipd/', '../../cipd/')
    # Write the file
    (examples_repo_root / 'build_overrides/pigweed_environment.gni').write_text(
        pwenvgni
    )

    # Set required GN args.
    pico_sdk_dir = ctx.package_root / 'pico_sdk'
    stm32cube_dir = ctx.package_root / 'stm32cube_f4'
    freertos_dir = ctx.package_root / 'freertos'
    nanopb_dir = ctx.package_root / 'nanopb'
    build.gn_gen(
        examples_ctx,
        dir_pigweed='"//../../.."',
        dir_pw_third_party_stm32cube_f4=f'"{stm32cube_dir}"',
        dir_pw_third_party_freertos=f'"{freertos_dir}"',
        dir_pw_third_party_nanopb=f'"{nanopb_dir}"',
        PICO_SRC_DIR=f'"{pico_sdk_dir}"',
    )
    build.ninja(examples_ctx, 'docs')

    # Copy rust docs from Bazel's out directory into where the GN build
    # put the main docs.
    rust_docs_bazel_dir = ctx.output_dir / 'bazel-bin/pw_rust/docs.rustdoc'
    rust_docs_output_dir = ctx.output_dir / 'docs/gen/docs/html/rustdoc'

    # Copy the doxygen html output to the main docs location.
    doxygen_html_gn_dir = ctx.output_dir / 'docs/doxygen/html'
    doxygen_html_output_dir = ctx.output_dir / 'docs/gen/docs/html/doxygen'

    # Copy the examples repo html output to the main docs location into
    # '/examples/'.
    examples_html_gn_dir = examples_repo_out / 'docs/gen/docs/html'
    examples_html_output_dir = ctx.output_dir / 'docs/gen/docs/html/examples'

    # Remove outputs to avoid including stale files from previous runs.
    shutil.rmtree(rust_docs_output_dir, ignore_errors=True)
    shutil.rmtree(doxygen_html_output_dir, ignore_errors=True)
    shutil.rmtree(examples_html_output_dir, ignore_errors=True)

    # Bazel generates files and directories without write permissions.  In
    # order to allow this rule to be run multiple times we use shutil.copyfile
    # for the actual copies to not copy permissions of files.
    shutil.copytree(
        rust_docs_bazel_dir,
        rust_docs_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )

    # Copy doxygen html outputs.
    shutil.copytree(
        doxygen_html_gn_dir,
        doxygen_html_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )

    # mkdir -p the example repo output dir and copy the files over.
    examples_html_output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        examples_html_gn_dir,
        examples_html_output_dir,
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )


gn_host_tools = PigweedGnGenNinja(
    name='gn_host_tools',
    ninja_targets=('host_tools',),
)


def _run_cmake(ctx: PresubmitContext, toolchain='host_clang') -> None:
    build.install_package(ctx, 'nanopb')
    build.install_package(ctx, 'emboss')

    env = None
    if 'clang' in toolchain:
        env = build.env_with_clang_vars()

    toolchain_path = ctx.root / 'pw_toolchain' / toolchain / 'toolchain.cmake'
    build.cmake(
        ctx,
        '--fresh',
        f'-DCMAKE_TOOLCHAIN_FILE={toolchain_path}',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=1',
        f'-Ddir_pw_third_party_nanopb={ctx.package_root / "nanopb"}',
        '-Dpw_third_party_nanopb_ADD_SUBDIRECTORY=ON',
        f'-Ddir_pw_third_party_emboss={ctx.package_root / "emboss"}',
        env=env,
    )


CMAKE_TARGETS = [
    'pw_apps',
    'pw_run_tests.modules',
]


@filter_paths(
    endswith=(*format_code.C_FORMAT.extensions, '.cmake', 'CMakeLists.txt')
)
def cmake_clang(ctx: PresubmitContext):
    _run_cmake(ctx, toolchain='host_clang')
    build.ninja(ctx, *CMAKE_TARGETS)
    build.gn_check(ctx)


@filter_paths(
    endswith=(*format_code.C_FORMAT.extensions, '.cmake', 'CMakeLists.txt')
)
def cmake_gcc(ctx: PresubmitContext):
    _run_cmake(ctx, toolchain='host_gcc')
    build.ninja(ctx, *CMAKE_TARGETS)
    build.gn_check(ctx)


@filter_paths(
    endswith=(*format_code.C_FORMAT.extensions, '.bazel', '.bzl', 'BUILD')
)
def bazel_test(ctx: PresubmitContext) -> None:
    """Runs bazel test on the entire repo."""
    build_bazel(
        ctx,
        'test',
        '--build_tag_filters=-requires_cxx_20',
        '--test_tag_filters=-requires_cxx_20',
        '--',
        '//...',
    )

    # Run tests for non-default config options

    # pw_rpc
    build_bazel(
        ctx,
        'test',
        '--//pw_rpc:config_override='
        '//pw_rpc:completion_request_callback_config_enabled',
        '--',
        '//pw_rpc/...',
    )

    # pw_grpc
    build_bazel(
        ctx,
        'test',
        '--//pw_rpc:config_override=//pw_grpc:pw_rpc_config',
        '--',
        '//pw_grpc/...',
    )


def bthost_package(ctx: PresubmitContext) -> None:
    target = '//pw_bluetooth_sapphire/fuchsia:infra'
    build_bazel(ctx, 'build', target)
    # Override the default test_tag_filters to ensure test targets tagged
    # "integration" are still run.
    build_bazel(
        ctx, 'test', '--test_tag_filters=-requires_cxx_20', f'{target}.test_all'
    )

    stdout_path = ctx.output_dir / 'bazel.manifest.stdout'
    with open(stdout_path, 'w') as outs:
        build_bazel(
            ctx,
            'build',
            '--output_groups=builder_manifest',
            target,
            stdout=outs,
        )

    manifest_path: Path | None = None
    for line in stdout_path.read_text().splitlines():
        line = line.strip()
        if line.endswith('infrabuilder_manifest.json'):
            manifest_path = Path(line)
            break
    else:
        raise PresubmitFailure('no manifest found in output')

    _LOG.debug('manifest: %s', manifest_path)
    shutil.copyfile(manifest_path, ctx.output_dir / 'builder_manifest.json')


@filter_paths(
    endswith=(
        *format_code.C_FORMAT.extensions,
        '.bazel',
        '.bzl',
        '.py',
        '.rs',
        'BUILD',
    )
)
def bazel_build(ctx: PresubmitContext) -> None:
    """Runs Bazel build for each supported platform."""
    # Build everything with the default flags.
    build_bazel(
        ctx,
        'build',
        '--build_tag_filters=-requires_cxx_20',
        '--',
        '//...',
    )

    # Mapping from Bazel platforms to targets which should be built for those
    # platforms.
    targets_for_config = {
        "lm3s6965evb": [
            "//pw_rust/...",
        ],
        "microbit": [
            "//pw_rust/...",
        ],
    }

    for cxxversion in ('c++17', 'c++20'):
        # Explicitly build for each supported C++ version.
        args = [ctx, 'build', f"--cxxopt=-std={cxxversion}"]
        if cxxversion == 'c++17':
            args += ['--build_tag_filters=-requires_cxx_20']
        args += ['--', '//...']
        build_bazel(*args)

        for config, targets in targets_for_config.items():
            build_bazel(
                ctx,
                'build',
                f'--config={config}',
                f"--cxxopt='-std={cxxversion}'",
                *targets,
            )

    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_freertos',
        '--//pw_thread_freertos:config_override=//pw_build:test_module_config',
        '//pw_build:module_config_test',
    )

    # Build upstream Pigweed for the rp2040.
    # First using the config.
    build_bazel(
        ctx,
        'build',
        '--config=rp2040',
        '//...',
        # Bazel will silently skip any incompatible targets in wildcard builds;
        # but we know that some end-to-end targets definitely should remain
        # compatible with this platform. So we list them explicitly. (If an
        # explicitly listed target is incompatible with the platform, Bazel
        # will return an error instead of skipping it.)
        '//pw_system:system_example',
    )
    # Then using the transition.
    #
    # This ensures that the rp2040_binary rule transition includes all required
    # backends.
    build_bazel(
        ctx,
        'build',
        '//pw_system:rp2040_system_example',
    )

    # Build upstream Pigweed for the Discovery board using STM32Cube.
    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_freertos',
        '//...',
        # Bazel will silently skip any incompatible targets in wildcard builds;
        # but we know that some end-to-end targets definitely should remain
        # compatible with this platform. So we list them explicitly. (If an
        # explicitly listed target is incompatible with the platform, Bazel
        # will return an error instead of skipping it.)
        '//pw_system:system_example',
    )

    # Build upstream Pigweed for the Discovery board using the baremetal
    # backends.
    build_bazel(
        ctx,
        'build',
        '--config=stm32f429i_baremetal',
        '//...',
    )

    # Build the fuzztest example.
    #
    # TODO: b/324652164 - This doesn't work on MacOS yet.
    if sys.platform != 'darwin':
        build_bazel(
            ctx,
            'build',
            '--config=fuzztest',
            '//pw_fuzzer/examples/fuzztest:metrics_fuzztest',
        )


def pw_transfer_integration_test(ctx: PresubmitContext) -> None:
    """Runs the pw_transfer cross-language integration test only.

    This test is not part of the regular bazel build because it's slow and
    intended to run in CI only.
    """
    build_bazel(
        ctx,
        'test',
        '//pw_transfer/integration_test:cross_language_small_test',
        '//pw_transfer/integration_test:cross_language_medium_read_test',
        '//pw_transfer/integration_test:cross_language_medium_write_test',
        '//pw_transfer/integration_test:cross_language_large_read_test',
        '//pw_transfer/integration_test:cross_language_large_write_test',
        '//pw_transfer/integration_test:multi_transfer_test',
        '//pw_transfer/integration_test:expected_errors_test',
        '//pw_transfer/integration_test:legacy_binaries_test',
        '--test_output=errors',
    )


#
# General presubmit checks
#


def _clang_system_include_paths(lang: str) -> list[str]:
    """Generate default system header paths.

    Returns the list of system include paths used by the host
    clang installation.
    """
    # Dump system include paths with preprocessor verbose.
    command = [
        'clang++',
        '-Xpreprocessor',
        '-v',
        '-x',
        f'{lang}',
        f'{os.devnull}',
        '-fsyntax-only',
    ]
    process = log_run(
        command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )

    # Parse the command output to retrieve system include paths.
    # The paths are listed one per line.
    output = process.stdout.decode(errors='backslashreplace')
    include_paths: list[str] = []
    for line in output.splitlines():
        path = line.strip()
        if os.path.exists(path):
            include_paths.append(f'-isystem{path}')

    return include_paths


def edit_compile_commands(
    in_path: Path, out_path: Path, func: Callable[[str, str, str], str]
) -> None:
    """Edit the selected compile command file.

    Calls the input callback on all triplets (file, directory, command) in
    the input compile commands database. The return value replaces the old
    compile command in the output database.
    """
    with open(in_path) as in_file:
        compile_commands = json.load(in_file)
        for item in compile_commands:
            item['command'] = func(
                item['file'], item['directory'], item['command']
            )
    with open(out_path, 'w') as out_file:
        json.dump(compile_commands, out_file, indent=2)


_EXCLUDE_FROM_COPYRIGHT_NOTICE: Sequence[str] = (
    # Configuration
    # keep-sorted: start
    r'MODULE.bazel.lock',
    r'\bDoxyfile$',
    r'\bPW_PLUGINS$',
    r'\bconstraint.list$',
    r'\bconstraint_hashes_darwin.list$',
    r'\bconstraint_hashes_linux.list$',
    r'\bconstraint_hashes_windows.list$',
    r'\bpython_base_requirements.txt$',
    r'\bupstream_requirements_darwin_lock.txt$',
    r'\bupstream_requirements_linux_lock.txt$',
    r'\bupstream_requirements_windows_lock.txt$',
    r'^(?:.+/)?\.bazelversion$',
    r'^pw_env_setup/py/pw_env_setup/cipd_setup/.cipd_version',
    # keep-sorted: end
    # Metadata
    # keep-sorted: start
    r'\b.*OWNERS.*$',
    r'\bAUTHORS$',
    r'\bLICENSE$',
    r'\bPIGWEED_MODULES$',
    r'\bgo.(mod|sum)$',
    r'\bpackage-lock.json$',
    r'\bpackage.json$',
    r'\brequirements.txt$',
    r'\byarn.lock$',
    r'^docker/tag$',
    r'^patches.json$',
    # keep-sorted: end
    # Data files
    # keep-sorted: start
    r'\.bin$',
    r'\.csv$',
    r'\.elf$',
    r'\.gif$',
    r'\.ico$',
    r'\.jpg$',
    r'\.json$',
    r'\.png$',
    r'\.svg$',
    r'\.vsix$',
    r'\.woff2',
    r'\.xml$',
    # keep-sorted: end
    # Documentation
    # keep-sorted: start
    r'\.md$',
    r'\.rst$',
    # keep-sorted: end
    # Generated protobuf files
    # keep-sorted: start
    r'\.pb\.c$',
    r'\.pb\.h$',
    r'\_pb2.pyi?$',
    # keep-sorted: end
    # Generated third-party files
    # keep-sorted: start
    r'\bthird_party/.*\.bazelrc$',
    r'\bthird_party/fuchsia/repo',
    r'\bthird_party/perfetto/repo/protos/perfetto/trace/perfetto_trace.proto',
    # keep-sorted: end
    # Diff/Patch files
    # keep-sorted: start
    r'\.diff$',
    r'\.patch$',
    # keep-sorted: end
    # Test data
    # keep-sorted: start
    r'\bpw_presubmit/py/test/owners_checks/',
    # keep-sorted: end
)

# Regular expression for the copyright comment. "\1" refers to the comment
# characters and "\2" refers to space after the comment characters, if any.
# All period characters are escaped using a replace call.
# pylint: disable=line-too-long
_COPYRIGHT = re.compile(
    r"""(#|//|::| \*|)( ?)Copyright 2\d{3} The Pigweed Authors
\1
\1\2Licensed under the Apache License, Version 2.0 \(the "License"\); you may not
\1\2use this file except in compliance with the License. You may obtain a copy of
\1\2the License at
\1
\1(?:\2    |\t)https://www.apache.org/licenses/LICENSE-2.0
\1
\1\2Unless required by applicable law or agreed to in writing, software
\1\2distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
\1\2WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
\1\2License for the specific language governing permissions and limitations under
\1\2the License.
""".replace(
        '.', r'\.'
    ),
    re.MULTILINE,
)
# pylint: enable=line-too-long

_SKIP_LINE_PREFIXES = (
    '#!',
    '#autoload',
    '#compdef',
    '@echo off',
    ':<<',
    '/*',
    ' * @jest-environment jsdom',
    ' */',
    '{#',  # Jinja comment block
    '# -*- coding: utf-8 -*-',
    '<!--',
)


def _read_notice_lines(file: TextIO) -> Iterable[str]:
    lines = iter(file)
    try:
        # Read until the first line of the copyright notice.
        line = next(lines)
        while line.isspace() or line.startswith(_SKIP_LINE_PREFIXES):
            line = next(lines)

        yield line

        for _ in range(12):  # The notice is 13 lines; read the remaining 12.
            yield next(lines)
    except StopIteration:
        return


@filter_paths(exclude=_EXCLUDE_FROM_COPYRIGHT_NOTICE)
def copyright_notice(ctx: PresubmitContext):
    """Checks that the Pigweed copyright notice is present."""
    errors = []

    for path in ctx.paths:
        if path.stat().st_size == 0:
            continue  # Skip empty files

        try:
            with path.open() as file:
                if not _COPYRIGHT.match(''.join(_read_notice_lines(file))):
                    errors.append(path)
        except UnicodeDecodeError as exc:
            raise PresubmitFailure(f'failed to read {path}') from exc

    if errors:
        _LOG.warning(
            '%s with a missing or incorrect copyright notice:\n%s',
            plural(errors, 'file'),
            '\n'.join(str(e) for e in errors),
        )
        raise PresubmitFailure


@filter_paths(endswith=format_code.CPP_SOURCE_EXTS)
def source_is_in_cmake_build_warn_only(ctx: PresubmitContext):
    """Checks that source files are in the CMake build."""

    _run_cmake(ctx)
    missing = SOURCE_FILES_FILTER_CMAKE_EXCLUDE.filter(
        build.check_compile_commands_for_files(
            ctx.output_dir / 'compile_commands.json',
            (f for f in ctx.paths if f.suffix in format_code.CPP_SOURCE_EXTS),
        )
    )
    if missing:
        _LOG.warning(
            'Files missing from CMake:\n%s',
            '\n'.join(str(f) for f in missing),
        )


def build_env_setup(ctx: PresubmitContext):
    if 'PW_CARGO_SETUP' not in os.environ:
        _LOG.warning('Skipping build_env_setup since PW_CARGO_SETUP is not set')
        return

    tmpl = ctx.root.joinpath('pw_env_setup', 'py', 'pyoxidizer.bzl.tmpl')
    out = ctx.output_dir.joinpath('pyoxidizer.bzl')

    with open(tmpl, 'r') as ins:
        cfg = ins.read().replace('${PW_ROOT}', str(ctx.root))
        with open(out, 'w') as outs:
            outs.write(cfg)

    call('pyoxidizer', 'build', cwd=ctx.output_dir)


def _valid_capitalization(word: str) -> bool:
    """Checks that the word has a capital letter or is not a regular word."""
    return bool(
        any(c.isupper() for c in word)  # Any capitalizatian (iTelephone)
        or not word.isalpha()  # Non-alphabetical (cool_stuff.exe)
        or shutil.which(word)
    )  # Matches an executable (clangd)


def commit_message_format(_: PresubmitContext):
    """Checks that the top commit's message is correctly formatted."""
    if git_repo.commit_author().endswith('gserviceaccount.com'):
        return

    lines = git_repo.commit_message().splitlines()

    # Show limits and current commit message in log.
    _LOG.debug('%-25s%+25s%+22s', 'Line limits', '72|', '72|')
    for line in lines:
        _LOG.debug(line)

    if not lines:
        _LOG.error('The commit message is too short!')
        raise PresubmitFailure

    # Ignore Gerrit-generated reverts.
    if (
        'Revert' in lines[0]
        and 'This reverts commit ' in git_repo.commit_message()
        and 'Reason for revert: ' in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring apparent Gerrit-generated revert')
        return

    # Ignore Gerrit-generated relands
    if (
        'Reland' in lines[0]
        and 'This is a reland of ' in git_repo.commit_message()
        and "Original change's description:" in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring apparent Gerrit-generated reland')
        return

    errors = 0

    if len(lines[0]) > 72:
        _LOG.warning(
            "The commit message's first line must be no longer than "
            '72 characters.'
        )
        _LOG.warning(
            'The first line is %d characters:\n  %s', len(lines[0]), lines[0]
        )
        errors += 1

    if lines[0].endswith('.'):
        _LOG.warning(
            "The commit message's first line must not end with a period:\n %s",
            lines[0],
        )
        errors += 1

    # Check that the first line matches the expected pattern.
    match = re.match(
        r'^(?:[.\w*/]+(?:{[\w* ,]+})?[\w*/]*|SEED-\d+): (?P<desc>.+)$', lines[0]
    )
    if not match:
        _LOG.warning('The first line does not match the expected format')
        _LOG.warning(
            'Expected:\n\n  module_or_target: The description\n\n'
            'Found:\n\n  %s\n',
            lines[0],
        )
        errors += 1
    elif not _valid_capitalization(match.group('desc').split()[0]):
        _LOG.warning(
            'The first word after the ":" in the first line ("%s") must be '
            'capitalized:\n  %s',
            match.group('desc').split()[0],
            lines[0],
        )
        errors += 1

    if len(lines) > 1 and lines[1]:
        _LOG.warning("The commit message's second line must be blank.")
        _LOG.warning(
            'The second line has %d characters:\n  %s', len(lines[1]), lines[1]
        )
        errors += 1

    # Ignore the line length check for Copybara imports so they can include the
    # commit hash and description for imported commits.
    if not errors and (
        'Copybara import' in lines[0]
        and 'GitOrigin-RevId:' in git_repo.commit_message()
    ):
        _LOG.warning('Ignoring Copybara import')
        return

    # Check that the lines are 72 characters or less.
    for i, line in enumerate(lines[2:], 3):
        # Skip any lines that might possibly have a URL, path, or metadata in
        # them.
        if any(c in line for c in ':/>'):
            continue

        # Skip any lines with non-ASCII characters.
        if not line.isascii():
            continue

        # Skip any blockquoted lines.
        if line.startswith('  '):
            continue

        if len(line) > 72:
            _LOG.warning(
                'Commit message lines must be no longer than 72 characters.'
            )
            _LOG.warning('Line %d has %d characters:\n  %s', i, len(line), line)
            errors += 1

    if errors:
        _LOG.error('Found %s in the commit message', plural(errors, 'error'))
        raise PresubmitFailure


@filter_paths(endswith=(*format_code.C_FORMAT.extensions, '.py'))
def static_analysis(ctx: PresubmitContext):
    """Runs all available static analysis tools."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'python.lint', 'static_analysis')
    build.gn_check(ctx)


_EXCLUDE_FROM_TODO_CHECK = (
    # keep-sorted: start
    r'.bazelrc$',
    r'.dockerignore$',
    r'.gitignore$',
    r'.pylintrc$',
    r'\bdocs/build_system.rst',
    r'\bdocs/code_reviews.rst',
    r'\bpw_assert_basic/basic_handler.cc',
    r'\bpw_assert_basic/public/pw_assert_basic/handler.h',
    r'\bpw_blob_store/public/pw_blob_store/flat_file_system_entry.h',
    r'\bpw_build/linker_script.gni',
    r'\bpw_build/py/pw_build/copy_from_cipd.py',
    r'\bpw_cpu_exception/basic_handler.cc',
    r'\bpw_cpu_exception_cortex_m/entry.cc',
    r'\bpw_cpu_exception_cortex_m/exception_entry_test.cc',
    r'\bpw_doctor/py/pw_doctor/doctor.py',
    r'\bpw_env_setup/util.sh',
    r'\bpw_fuzzer/fuzzer.gni',
    r'\bpw_i2c/BUILD.gn',
    r'\bpw_i2c/public/pw_i2c/register_device.h',
    r'\bpw_kvs/flash_memory.cc',
    r'\bpw_kvs/key_value_store.cc',
    r'\bpw_log_basic/log_basic.cc',
    r'\bpw_package/py/pw_package/packages/chromium_verifier.py',
    r'\bpw_protobuf/encoder.cc',
    r'\bpw_rpc/docs.rst',
    r'\bpw_watch/py/pw_watch/watch.py',
    r'\btargets/mimxrt595_evk/BUILD.bazel',
    r'\btargets/stm32f429i_disc1/boot.cc',
    r'\bthird_party/chromium_verifier/BUILD.gn',
    # keep-sorted: end
)


@filter_paths(exclude=_EXCLUDE_FROM_TODO_CHECK)
def todo_check_with_exceptions(ctx: PresubmitContext):
    """Check that non-legacy TODO lines are valid."""  # todo-check: ignore
    todo_check.create(todo_check.BUGS_OR_USERNAMES)(ctx)


@filter_paths(file_filter=format_code.OWNERS_CODE_FORMAT.filter)
def owners_lint_checks(ctx: PresubmitContext):
    """Runs OWNERS linter."""
    owners_checks.presubmit_check(ctx.paths)


SOURCE_FILES_FILTER = FileFilter(
    endswith=_BUILD_FILE_FILTER.endswith,
    suffix=('.bazel', '.bzl', '.gn', '.gni', *_BUILD_FILE_FILTER.suffix),
    exclude=(
        r'zephyr.*',
        r'android.*',
        r'\.black.toml',
        r'pyproject.toml',
    ),
)

SOURCE_FILES_FILTER_GN_EXCLUDE = FileFilter(
    exclude=(
        # keep-sorted: start
        r'\bpw_bluetooth_sapphire/fuchsia',
        # keep-sorted: end
    ),
)

SOURCE_FILES_FILTER_CMAKE_EXCLUDE = FileFilter(
    exclude=(
        # keep-sorted: start
        r'\bpw_bluetooth_sapphire/fuchsia',
        # keep-sorted: end
    ),
)

#
# Presubmit check programs
#

OTHER_CHECKS = (
    # keep-sorted: start
    bazel_test,
    bthost_package,
    build.gn_gen_check,
    cmake_clang,
    cmake_gcc,
    coverage,
    # TODO: b/234876100 - Remove once msan is added to all_sanitizers().
    cpp_checks.msan,
    docs_build,
    gitmodules.create(gitmodules.Config(allow_submodules=False)),
    gn_all,
    gn_clang_build,
    gn_combined_build_check,
    module_owners.presubmit_check(),
    npm_presubmit.npm_test,
    pw_transfer_integration_test,
    python_checks.update_upstream_python_constraints,
    python_checks.vendor_python_wheels,
    shell_checks.shellcheck,
    # TODO(hepler): Many files are missing from the CMake build. Add this check
    # to lintformat when the missing files are fixed.
    source_in_build.cmake(SOURCE_FILES_FILTER, _run_cmake),
    static_analysis,
    stm32f429i,
    todo_check.create(todo_check.BUGS_OR_USERNAMES),
    zephyr_build,
    # keep-sorted: end
)

ARDUINO_PICO = (
    # Skip gn_teensy_build if running on mac-arm64.
    # There are no arm specific tools packages available upstream:
    # https://www.pjrc.com/teensy/package_teensy_index.json
    gn_teensy_build
    if not (sys.platform == 'darwin' and platform.machine() == 'arm64')
    else (),
    gn_pico_build,
    gn_pw_system_demo_build,
)

INTERNAL = (gn_mimxrt595_build, gn_mimxrt595_freertos_build)

# The misc program differs from other_checks in that checks in the misc
# program block CQ on Linux.
MISC = (
    # keep-sorted: start
    gn_chre_googletest_nanopb_sapphire_build,
    # keep-sorted: end
)

SANITIZERS = (cpp_checks.all_sanitizers(),)

SECURITY = (
    # keep-sorted: start
    gn_crypto_mbedtls_build,
    gn_crypto_micro_ecc_build,
    gn_software_update_build,
    # keep-sorted: end
)

FUZZ = (gn_fuzz_build, oss_fuzz_build)

_LINTFORMAT = (
    commit_message_format,
    copyright_notice,
    format_code.presubmit_checks(),
    inclusive_language.presubmit_check.with_filter(
        exclude=(
            r'\byarn.lock$',
            r'\bpackage-lock.json$',
        )
    ),
    cpp_checks.pragma_once,
    build.bazel_lint,
    owners_lint_checks,
    source_in_build.gn(SOURCE_FILES_FILTER).with_file_filter(
        SOURCE_FILES_FILTER_GN_EXCLUDE
    ),
    source_is_in_cmake_build_warn_only,
    javascript_checks.eslint if shutil.which('npm') else (),
    json_check.presubmit_check,
    keep_sorted.presubmit_check,
    todo_check_with_exceptions,
)

LINTFORMAT = (
    _LINTFORMAT,
    # This check is excluded from _LINTFORMAT because it's not quick: it issues
    # a bazel query that pulls in all of Pigweed's external dependencies
    # (https://stackoverflow.com/q/71024130/1224002). These are cached, but
    # after a roll it can be quite slow.
    source_in_build.bazel(SOURCE_FILES_FILTER),
    python_checks.check_python_versions,
    python_checks.gn_python_lint,
)

QUICK = (
    _LINTFORMAT,
    gn_quick_build_check,
)

FULL = (
    _LINTFORMAT,
    gn_combined_build_check,
    gn_host_tools,
    bazel_test,
    bazel_build,
    python_checks.gn_python_check,
    python_checks.gn_python_test_coverage,
    python_checks.check_upstream_python_constraints,
    build_env_setup,
)

PROGRAMS = Programs(
    # keep-sorted: start
    arduino_pico=ARDUINO_PICO,
    full=FULL,
    fuzz=FUZZ,
    internal=INTERNAL,
    lintformat=LINTFORMAT,
    misc=MISC,
    other_checks=OTHER_CHECKS,
    quick=QUICK,
    sanitizers=SANITIZERS,
    security=SECURITY,
    # keep-sorted: end
)


def parse_args() -> argparse.Namespace:
    """Creates an argument parser and parses arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    cli.add_arguments(parser, PROGRAMS, 'quick')
    parser.add_argument(
        '--install',
        action='store_true',
        help='Install the presubmit as a Git pre-push hook and exit.',
    )

    return parser.parse_args()


def run(install: bool, **presubmit_args) -> int:
    """Entry point for presubmit."""

    if install:
        install_git_hook(
            'pre-push',
            [
                'python',
                '-m',
                'pw_presubmit.pigweed_presubmit',
                '--base',
                'origin/main..HEAD',
                '--program',
                'quick',
            ],
        )
        return 0

    return cli.run(**presubmit_args)


def main() -> int:
    """Run the presubmit for the Pigweed repository."""
    return run(**vars(parse_args()))


if __name__ == '__main__':
    try:
        # If pw_cli is available, use it to initialize logs.
        from pw_cli import log  # pylint: disable=ungrouped-imports

        log.install(logging.INFO)
    except ImportError:
        # If pw_cli isn't available, display log messages like a simple print.
        logging.basicConfig(format='%(message)s', level=logging.INFO)

    sys.exit(main())
