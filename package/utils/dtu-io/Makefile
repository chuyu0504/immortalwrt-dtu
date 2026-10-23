include $(TOPDIR)/rules.mk

PKG_NAME:=dtu-io
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/dtu-io
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=DTU board I/O service
  DEPENDS:=+libubus +libubox +libuci
endef

define Package/dtu-io/description
 Single owner of board digital I/O, with GPIO character-device v2, UCI
 configuration and a ubus API. Hardware profiles restrict access to verified
 terminal pins. Analog channels without a verified driver are unavailable.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/dtu-io ./src/dtu-io.c \
		$(TARGET_LDFLAGS) -lubus -lubox -luci
endef

define Package/dtu-io/conffiles
/etc/config/dtu_io
endef

define Package/dtu-io/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/dtu-io $(1)/usr/sbin/dtu-io
	$(INSTALL_BIN) ./files/dtu-io.init $(1)/etc/init.d/dtu-io
	$(INSTALL_CONF) ./files/dtu_io.config $(1)/etc/config/dtu_io
endef

$(eval $(call BuildPackage,dtu-io))
