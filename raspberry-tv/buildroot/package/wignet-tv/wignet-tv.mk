################################################################################
#
# wignet-tv: the client in raspberry-tv, installed as on Pi OS
#
################################################################################

WIGNET_TV_VERSION = local
WIGNET_TV_SITE = $(BR2_EXTERNAL_WIGNET_PATH)/..
WIGNET_TV_SITE_METHOD = local
WIGNET_TV_LICENSE = GPL-3.0-or-later

WIGNET_TV_PORTAL = $(BR2_EXTERNAL_WIGNET_PATH)/../../esp32/portal
WIGNET_TV_LIB = $(TARGET_DIR)/usr/local/lib/wignet-tv

define WIGNET_TV_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(WIGNET_TV_LIB)/portal/static
	$(INSTALL) -m 0755 $(addprefix $(@D)/,wignet_tv.py wifi_setup.py cec.py tv_power.py \
		boot-splash.sh) $(WIGNET_TV_LIB)/
	$(INSTALL) -m 0644 $(addprefix $(@D)/,wifi_wpa.py sound_list.py sound_player.py \
		tune_render.py mml.py tv_ota.py) $(WIGNET_TV_LIB)/
	python3 $(BR2_EXTERNAL_WIGNET_PATH)/../tv_version.py >$(WIGNET_TV_LIB)/VERSION
	$(INSTALL) -m 0644 $(WIGNET_TV_PORTAL)/wifi.html $(WIGNET_TV_PORTAL)/connect.html \
		$(WIGNET_TV_LIB)/portal/
	$(INSTALL) -m 0644 $(WIGNET_TV_PORTAL)/portal.css $(WIGNET_TV_PORTAL)/wignet-portal.css \
		$(WIGNET_TV_PORTAL)/portal.js $(WIGNET_TV_LIB)/portal/static/
endef

$(eval $(generic-package))
