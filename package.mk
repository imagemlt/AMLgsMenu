# SPDX-License-Identifier: GPL-3.0-or-later

PKG_NAME="amlgsmenu"
PKG_VERSION="759bdfaafff5a4b2a3633d255bdedd55b68deedb"
PKG_LICENSE="proprietary"
PKG_SITE="https://github.com/imagemlt/AMLgsMenu"
PKG_URL="${PKG_SITE}.git"
PKG_GIT_CLONE_BRANCH="main"
PKG_DEPENDS_TARGET="toolchain SDL2"
PKG_LONGDESC="Transparent OSD and configuration menu for Amlogic GLES/fbdev targets using SDL2 + Dear ImGui."
PKG_TOOLCHAIN="cmake"

post_unpack() {
  git -C ${PKG_BUILD} submodule update --init --recursive
}

pre_configure_target() {
  cd ${PKG_BUILD}
  rm -rf .${TARGET_NAME}
}

PKG_CMAKE_OPTS_TARGET="-DIMGUI_ROOT=${PKG_BUILD}/third_party/imgui -DAML_ENABLE_GLES=ON"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/.${TARGET_NAME}/aml_gs_menu ${INSTALL}/usr/bin/
}
