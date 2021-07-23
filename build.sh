#!/bin/bash
echo "Getting root privledges . . ."
sudo echo "Done!"
echo
mkdir build
cd build
meson --prefix=/usr      \
      --sysconfdir=/etc  \
      -Dselinux=false    \
      -Dpackagekit=false \
      ..
ninja && sudo ninja install
cd ..
rm -rf build
