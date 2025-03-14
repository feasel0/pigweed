.. _module-pw_build-bazel:

Bazel
=====
.. pigweed-module-subpage::
   :name: pw_build

Bazel is currently very experimental, and only builds for host and ARM Cortex-M
microcontrollers.

.. _module-pw_build-bazel-wrapper-rules:

Wrapper rules
-------------
The common configuration for Bazel for all modules is in the ``pigweed.bzl``
file. The built-in Bazel rules ``cc_binary``, ``cc_test`` and ``py_test`` are
wrapped with ``pw_cc_binary``, ``pw_cc_test`` and ``pw_py_test``.

.. _module-pw_build-bazel-pw_linker_script:

pw_linker_script
----------------
In addition to wrapping the built-in rules, Pigweed also provides a custom
rule for handling linker scripts with Bazel. e.g.

.. code-block:: python

   pw_linker_script(
     name = "some_linker_script",
     linker_script = ":some_configurable_linker_script.ld",
     defines = [
         "PW_BOOT_FLASH_BEGIN=0x08000200",
         "PW_BOOT_FLASH_SIZE=1024K",
         "PW_BOOT_HEAP_SIZE=112K",
         "PW_BOOT_MIN_STACK_SIZE=1K",
         "PW_BOOT_RAM_BEGIN=0x20000000",
         "PW_BOOT_RAM_SIZE=192K",
         "PW_BOOT_VECTOR_TABLE_BEGIN=0x08000000",
         "PW_BOOT_VECTOR_TABLE_SIZE=512",
     ],
     deps = [":some_header_library"],
   )

   # You can include headers provided by targets specified in deps.
   cc_library(
     name = "some_header_library",
     hdrs = ["test_header.h"],
     includes = ["."],
   )

   # You can include the linker script in the deps.
   cc_binary(
     name = "some_binary",
     srcs = ["some_source.cc"],
     deps = [":some_linker_script"],
   )

   # Alternatively, you can use additional_linker_inputs and linkopts. This
   # allows you to explicitly specify the command line order of linker scripts,
   # and may be useful if your project defines more than one.
   cc_binary(
     name = "some_binary",
     srcs = ["some_source.cc"],
     additional_linker_inputs = [":some_linker_script"],
     linkopts = ["-T $(location :some_linker_script)"],
   )

.. _module-pw_build-bazel-pw_facade:

pw_facade
---------
In Bazel, a :ref:`facade <docs-module-structure-facades>` module has a few
components:

#. The **facade target**, i.e. the interface to the module. This is what
   *backend implementations* depend on to know what interface they're supposed
   to implement.

#. The **library target**, i.e. both the facade (interface) and backend
   (implementation). This is what *users of the module* depend on. It's a
   regular ``cc_library`` that exposes the same headers as the facade, but
   has a dependency on the "backend label flag" (discussed next). It may also
   include some source files (if these are backend-independent).

   Both the facade and library targets are created using the
   ``pw_facade`` macro. For example, consider the following
   macro invocation:

   .. code-block:: python

      pw_facade(
          name = "binary_semaphore",
          # A backend-independent source file.
          srcs = [
              "binary_semaphore.cc",
          ],
          # The facade header.
          hdrs = [
              "public/pw_sync/binary_semaphore.h",
          ],
          # Dependencies of this header.
          deps = [
              "//pw_chrono:system_clock",
              "//pw_preprocessor",
          ],
          # The backend, hidden behind a label_flag; see below.
          backend = [
              ":binary_semaphore_backend",
          ],
      )

   This macro expands to both the library target, named ``binary_semaphore``,
   and the facade target, named ``binary_semaphore.facade``.

#. The **backend label flag**. This is a `label_flag
   <https://bazel.build/extending/config#label-typed-build-settings>`_: a
   dependency edge in the build graph that can be overridden by downstream projects.

#. The **backend target** implements a particular backend for a facade. It's
   just a plain ``cc_library``, with a dependency on the facade target. For example,

   .. code-block:: python

      cc_library(
          name = "binary_semaphore",
          srcs = [
              "binary_semaphore.cc",
          ],
          hdrs = [
              "public/pw_sync_stl/binary_semaphore_inline.h",
              "public/pw_sync_stl/binary_semaphore_native.h",
              "public_overrides/pw_sync_backend/binary_semaphore_inline.h",
              "public_overrides/pw_sync_backend/binary_semaphore_native.h",
          ],
          includes = [
              "public",
              "public_overrides",
          ],
          deps = [
              # Dependencies of the backend's headers and sources.
              "//pw_assert",
              "//pw_chrono:system_clock",
              # A dependency on the facade target, which defines the interface
              # this backend target implements.
              "//pw_sync:binary_semaphore.facade",
          ],
      )

The backend label flag should point at the backend target. Typically, the
backend you want to use depends on the platform you are building for. See the
:ref:`docs-build_system-bazel_configuration` for advice on how to set this up.

pw_cc_blob_library
------------------
The ``pw_cc_blob_library`` rule is useful for embedding binary data into a
program. The rule takes in a mapping of symbol names to file paths, and
generates a set of C++ source and header files that embed the contents of the
passed-in files as arrays of ``std::byte``.

The blob byte arrays are constant initialized and are safe to access at any
time, including before ``main()``.

``pw_cc_blob_library`` is also available in the :ref:`GN <module-pw_build-cc_blob_library>`
and CMake builds.

Arguments
^^^^^^^^^
* ``blobs``: A list of ``pw_cc_blob_info`` targets, where each target
  corresponds to a binary blob to be transformed from file to byte array. This
  is a required field. ``pw_cc_blob_info`` attributes include:

  * ``symbol_name``: The C++ symbol for the byte array.
  * ``file_path``: The file path for the binary blob.
  * ``linker_section``: If present, places the byte array in the specified
    linker section.
  * ``alignas``: If present, uses the specified string verbatim in
    the ``alignas()`` specifier for the byte array.

* ``out_header``: The header file to generate. Users will include this file
  exactly as it is written here to reference the byte arrays.
* ``namespace``: C++ namespace to place the generated blobs within.
* ``alwayslink``: Whether this library should always be linked. Defaults to false.

Example
^^^^^^^
**BUILD.bazel**

.. code-block:: python

   pw_cc_blob_info(
     name = "foo_blob",
     file_path = "foo.bin",
     symbol_name = "kFooBlob",
   )

   pw_cc_blob_info(
     name = "bar_blob",
     file_path = "bar.bin",
     symbol_name = "kBarBlob",
     linker_section = ".bar_section",
   )

   pw_cc_blob_library(
     name = "foo_bar_blobs",
     blobs = [
       ":foo_blob",
       ":bar_blob",
     ],
     out_header = "my/stuff/foo_bar_blobs.h",
     namespace = "my::stuff",
   )

.. note:: If the binary blobs are generated as part of the build, be sure to
          list them as deps to the pw_cc_blob_library target.

**Generated Header**

.. code-block::

   #pragma once

   #include <array>
   #include <cstddef>

   namespace my::stuff {

   extern const std::array<std::byte, 100> kFooBlob;

   extern const std::array<std::byte, 50> kBarBlob;

   }  // namespace my::stuff

**Generated Source**

.. code-block::

   #include "my/stuff/foo_bar_blobs.h"

   #include <array>
   #include <cstddef>

   #include "pw_preprocessor/compiler.h"

   namespace my::stuff {

   const std::array<std::byte, 100> kFooBlob = { ... };

   PW_PLACE_IN_SECTION(".bar_section")
   const std::array<std::byte, 50> kBarBlob = { ... };

   }  // namespace my::stuff

.. _module-pw_build-bazel-pw_cc_binary_with_map:

pw_cc_binary_with_map
---------------------
The ``pw_cc_binary_with_map`` rule can be used to build a binary like
``cc_binary`` does but also generate a .map file from the linking step.

.. code-block:: python

   pw_cc_binary_with_map(
     name = "test",
     srcs = ["empty_main.cc"],
   )

This should result in a ``test.map`` file generated next to the ``test`` binary.

Note that it's only partially compatible with the ``cc_binary`` interface and
certain things are not implemented like make variable substitution.

.. _module-pw_build-bazel-pw_elf_to_bin:

pw_elf_to_bin
-------------
The ``pw_elf_to_bin`` rule takes in a binary executable target and produces a
file using the ``-Obinary`` option to ``objcopy``. This is only suitable for use
with binaries where all the segments are non-overlapping. A common use case for
this type of file is booting directly on hardware with no bootloader.

.. code-block:: python

   load("@pigweed//pw_build:binary_tools.bzl", "pw_elf_to_bin")

   pw_elf_to_bin(
     name = "bin",
     elf_input = ":main",
     bin_out = "main.bin",
   )

.. _module-pw_build-bazel-pw_elf_to_dump:

pw_elf_to_dump
--------------
The ``pw_elf_to_dump`` rule takes in a binary executable target and produces a
text file containing the output of the toolchain's ``objdump -xd`` command. This
contains the full binary layout, symbol table and disassembly which is often
useful when debugging embedded firmware.

.. code-block:: python

   load("@pigweed//pw_build:binary_tools.bzl", "pw_elf_to_dump")

   pw_elf_to_dump(
     name = "dump",
     elf_input = ":main",
     dump_out = "main.dump",
   )

Platform compatibility rules
----------------------------
Macros and rules related to platform compatibility are provided in
``//pw_build:compatibility.bzl``.

.. _module-pw_build-bazel-boolean_constraint_value:

boolean_constraint_value
^^^^^^^^^^^^^^^^^^^^^^^^
This macro is syntactic sugar for declaring a `constraint setting
<https://bazel.build/reference/be/platforms-and-toolchains#constraint_setting>`__
with just two possible `constraint values
<https://bazel.build/reference/be/platforms-and-toolchains#constraint_value>`__.
The only exposed target is the ``constraint_value`` corresponding to ``True``;
the default value of the setting is ``False``.

This macro is meant to simplify declaring
:ref:`docs-bazel-compatibility-module-specific`.

host_backend_alias
^^^^^^^^^^^^^^^^^^
An alias that resolves to the backend for host platforms. This is useful when
declaring a facade that provides a default backend for host platform use.

Flag merging rules
------------------
Macros that help with using platform-based flags are in
``//pw_build:merge_flags.bzl``. These are useful, for example, when you wish to
:ref:`docs-bazel-compatibility-facade-backend-dict`.

Miscellaneous utilities
-----------------------

.. _module-pw_build-bazel-empty_cc_library:

empty_cc_library
^^^^^^^^^^^^^^^^
This empty library is used as a placeholder for label flags that need to point
to a library of some kind, but don't actually need the dependency to amount to
anything.

default_link_extra_lib
^^^^^^^^^^^^^^^^^^^^^^
This library groups together all libraries commonly required at link time by
Pigweed modules. See :ref:`docs-build_system-bazel_link-extra-lib` for more
details.

unspecified_backend
^^^^^^^^^^^^^^^^^^^
A special target used instead of a cc_library as the default condition in
backend multiplexer select statements to signal that a facade is in an
unconfigured state. This produces better error messages than e.g. using an
invalid label.

Toolchains and platforms
------------------------
Pigweed provides clang-based host toolchains for Linux and Mac Arm gcc
toolchain. The clang-based Linux and Arm gcc toolchains are entirely hermetic.
We don't currently provide a host toolchain for Windows.
