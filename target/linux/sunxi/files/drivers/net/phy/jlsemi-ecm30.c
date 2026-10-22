// SPDX-License-Identifier: GPL-2.0-only
/* ECM30's observed JLSemi PHY needs to supply the RMII reference clock. */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>

#define ECM30_PHY_ID		0x937c4024
#define JLSEMI_PAGE_SELECT	31
#define JLSEMI_RMII_PAGE		7
#define JLSEMI_RMII_MODE		16
#define JLSEMI_REFCLK_INPUT	BIT(12)

static bool jlsemi_ecm30_clock_output(struct phy_device *phydev)
{
	return of_property_read_bool(phydev->mdio.dev.of_node,
				     "jlsemi,rmii-refclk-out");
}

static int jlsemi_ecm30_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, JLSEMI_PAGE_SELECT);
}

static int jlsemi_ecm30_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, JLSEMI_PAGE_SELECT, page);
}

static int jlsemi_ecm30_soft_reset(struct phy_device *phydev)
{
	int ret;

	if (!jlsemi_ecm30_clock_output(phydev))
		return 0;
	/* This PHY does not complete BMCR_RESET while BMCR_PDOWN remains set.
	 * Generic reset preserves that bit, so wake it before every reopen.
	 */
	ret = phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);
	if (ret)
		return ret;
	return genphy_soft_reset(phydev);
}

static int jlsemi_ecm30_config_init(struct phy_device *phydev)
{
	int ret;

	if (!jlsemi_ecm30_clock_output(phydev))
		return 0;
	if (phydev->interface != PHY_INTERFACE_MODE_RMII)
		return -EINVAL;

	/* P7:R16 bit 12: 0 = PHY supplies 50 MHz, 1 = PHY receives 50 MHz.
	 * Preserve interface selection, skew and every unrelated register bit.
	 * The paged helper restores the previous page on success and error.
	 */
	ret = phy_modify_paged(phydev, JLSEMI_RMII_PAGE, JLSEMI_RMII_MODE,
			      JLSEMI_REFCLK_INPUT, 0);
	if (ret < 0)
		return ret;
	/* The MAC software reset requires a stable PHY receive clock. */
	msleep(100);
	return 0;
}

static struct phy_driver jlsemi_ecm30_drivers[] = {
	{
		PHY_ID_MATCH_EXACT(ECM30_PHY_ID),
		.name = "JLSemi ECM30 RMII PHY",
		.get_features = genphy_read_abilities,
		.soft_reset = jlsemi_ecm30_soft_reset,
		.config_init = jlsemi_ecm30_config_init,
		.config_aneg = genphy_config_aneg,
		.read_status = genphy_read_status,
		.suspend = genphy_suspend,
		.resume = genphy_resume,
		.read_page = jlsemi_ecm30_read_page,
		.write_page = jlsemi_ecm30_write_page,
	},
};
module_phy_driver(jlsemi_ecm30_drivers);

static const struct mdio_device_id __maybe_unused jlsemi_ecm30_ids[] = {
	{ PHY_ID_MATCH_EXACT(ECM30_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, jlsemi_ecm30_ids);
MODULE_DESCRIPTION("ECM30 JLSemi RMII reference clock initialization");
MODULE_LICENSE("GPL");
