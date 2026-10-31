<!-- SPDX-License-Identifier: Apache-2.0 -->
# OpenXJ380 system configuration

`/config` stores native, system-wide OpenXJ380 configuration. It is an ordinary
directory on the mounted system root, and is required in newly staged system
images and installer temporary roots. This guide is installed as
`/config/README.md`.

Use a component-specific subdirectory, such as `/config/<component>/`, for new
native global settings. Keep user preferences in the corresponding user
directory. Store temporary files, logs, caches, and other changing runtime data
outside `/config`.

`/etc` remains available for Linux/POSIX compatibility and package configuration.
`/system/config` remains available to existing components that already use that
path. These directories are separate: there is no automatic alias, fallback,
or migration between them.

Rebuilding an image installs the current directory layout and guide into that
new image. It does not migrate configuration in an existing installation.
