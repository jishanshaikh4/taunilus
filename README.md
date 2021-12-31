# Taunilus

A powerful file-manager, Taunilus, is a fork of Nautilus aka [Files](https://wiki.gnome.org/Apps/Files) app, a file browser for GNOME (Ubuntu).

## Dependencies

- glib_ver, version: '>= 2.67.1'
- gio-2.0, version: glib_ver
- gio-unix-2.0, version: glib_ver
- glib-2.0, version: glib_ver
- gmodule-no-export-2.0, version: glib_ver
- gnome-autoar-0, version: '>= 0.4.0'
- gnome-desktop-3.0, version: '>= 3.0.0'
- gtk+-3.0, version: '>= 3.22.27'
- libhandy-1, version: '>= 1.1.90'
- libm, version: 'latest'
- libportal, version: '>= 0.5'
- libportal-gtk3, version: '>= 0.5'
- tracker-sparql, version: 'latest'
- libxml-2.0, version: '>= 2.7.8'

Additional dependencies:

- If using `selinux`, then {(libselinux, version: '>= 2.0')}
- If using `extensions`, then {('gexiv2', version: '>= 0.14.0'), ('gstreamer-tag-1.0'), ('gstreamer-pbutils-1.0')}

Runtime dependencies:

- [Bubblewrap](https://github.com/projectatomic/bubblewrap) installed. tag:security.
- [Tracker (including tracker-miners)](https://gitlab.gnome.org/GNOME/tracker) properly set up and with all features enabled. tag:indexing.

## Build Instructions on Ubuntu based distros

### Using GNOME Builder IDE
- Install GNOME Builder IDE (if not installed) as `sudo apt-get install gnome-builder`.
- Open Taunilus folder as a project
- Click Build icon on middle-top (It will prompt to install GTK SDKs and some dependencies)
- Click Run or (Ctrl+F5) to launch the application

Hint: There is a section named `Build Issues` in the middle of top-left icons. There you can find all the following commands to execute:

- Create flatpak workspace
- Prepare build directory
- Initialize git submodules
- Download dependencies
- Build dependencies
- Configure project
- Build project
- Install project
- Finalize flatpak build
- Export staging directory
- Create flatpak bundle

If you want to re-configure any of them, modify `meson.build` located in the root directory.

If you are not familiar with building GNOME apps with flatpak and Builder, check out the GNOME [build guide](https://wiki.gnome.org/Newcomers/BuildProject).

### Using Command Line

- `bash build.sh`. In this case, you'll have to handle the dependencies manually (Example: gexiv2).

## LICENSE

GPL v3. Please see LICENSE file in the root directory.
