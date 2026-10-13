#
# OpenWrt package for libpeer, see "OpenWrt Package" in the libpeer README
#
include $(TOPDIR)/rules.mk

PKG_NAME:=libpeer
PKG_VERSION:=0.0.3
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE

# libpeer source tree with its git submodules, defaults to the tree holding this Makefile
# (or the package directory symlinked to it), set it when the Makefile is copied elsewhere
LIBPEER_SRC_DIR ?= $(abspath $(CURDIR)/..)

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)
CMAKE_BINARY_SUBDIR:=openwrt-build
CMAKE_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/libpeer
  SECTION:=libs
  CATEGORY:=Oray
  TITLE:=WebRTC library for IoT and embedded devices
  URL:=https://github.com/sepfy/libpeer
  DEPENDS:=+libpthread
endef

define Package/libpeer/description
  libpeer is a WebRTC implementation in C. mbedtls, libsrtp, usrsctp
  and cJSON are linked statically into libpeer.so.
endef

define Package/libpeer-sample
  SECTION:=net
  CATEGORY:=Oray
  TITLE:=libpeer sample
  URL:=https://github.com/sepfy/libpeer
  DEPENDS:=+libpeer +libpthread
endef

define Package/libpeer-sample/description
  Streams test.264 and test.wav from the working directory to browsers,
  see "Getting Started with Generic Example" in the libpeer README.
endef

CMAKE_OPTIONS += -DCMAKE_TOOLCHAIN_FILE=$(PKG_BUILD_DIR)/openwrt-toolchain.cmake

define Build/Prepare
	@if [ ! -f "$(LIBPEER_SRC_DIR)/third_party/mbedtls/CMakeLists.txt" ]; then \
		echo "libpeer source with submodules not found in $(LIBPEER_SRC_DIR)," \
			"set LIBPEER_SRC_DIR or run git submodule update --init --recursive" >&2; \
		exit 1; \
	fi
	mkdir -p $(PKG_BUILD_DIR)
	tar -C "$(LIBPEER_SRC_DIR)" --exclude-vcs --exclude=./build --exclude=./openwrt-build \
		--exclude=./examples/esp32/build --exclude=./examples/esp32/managed_components \
		--exclude='./*.264' --exclude='./*.wav' --exclude='./*.mp4' -cf - . | \
		tar -C $(PKG_BUILD_DIR) -xf -
endef

# mbedtls, libsrtp, usrsctp and cJSON are built as CMake ExternalProjects, which only
# get the cross compiler through a toolchain file. mbedtls generates sources with the
# host python3, which needs the jsonschema and jinja2 modules
define Build/Configure
	( echo 'set(CMAKE_SYSTEM_NAME Linux)'; \
	  echo 'set(CMAKE_SYSTEM_PROCESSOR $(ARCH))'; \
	  echo 'set(CMAKE_C_COMPILER "$(CMAKE_C_COMPILER)")'; \
	  echo 'set(CMAKE_CXX_COMPILER "$(CMAKE_CXX_COMPILER)")'; \
	  echo 'set(CMAKE_AR "$(CMAKE_AR)" CACHE FILEPATH "")'; \
	  echo 'set(CMAKE_RANLIB "$(CMAKE_RANLIB)" CACHE FILEPATH "")'; \
	  echo 'set(CMAKE_STRIP "$(CMAKE_STRIP)" CACHE FILEPATH "")'; \
	  echo 'set(CMAKE_FIND_ROOT_PATH "$(CMAKE_FIND_ROOT_PATH)")'; \
	  echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)'; \
	  echo 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)'; \
	  echo 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)'; \
	  echo 'set(Python3_EXECUTABLE "$(STAGING_DIR_HOST)/bin/python3")'; \
	) > $(PKG_BUILD_DIR)/openwrt-toolchain.cmake
	$(call Build/Configure/Default)
endef

define Package/libpeer/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libpeer.so* $(1)/usr/lib/
endef

define Package/libpeer-sample/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(CMAKE_BINARY_DIR)/examples/generic/sample $(1)/usr/bin/libpeer-sample
endef

$(eval $(call BuildPackage,libpeer))
$(eval $(call BuildPackage,libpeer-sample))
