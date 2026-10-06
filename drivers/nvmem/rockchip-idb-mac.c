// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip RK3288 IDB MAC address
 *
 * Reads the factory-programmed LAN MAC address from the Rockchip IDB
 * (the loader block at LBA 64).  The MAC lives in the last six bytes of
 * the IDB "SN sector" (IDB sector 3, i.e. LBA 64 + 3 = 67), matching the
 * vendor kernel's eth_mac_idb()/eth_mac_read_from_IDB():
 *
 *	GetSNSectorInfo(buf);          // 512 bytes from IDB sector 3
 *	for (i = 506; i <= 511; i++)
 *		mac[i - 506] = buf[i];
 *
 * IDB sectors 2 and 3 are stored RC4-encrypted on the flash (decrypted by
 * u-boot's P_RC4() before use), so the sector is decrypted here first with
 * the fixed Rockchip IDB key.
 *
 * The value is exposed as a read-only nvmem cell so the GMAC (stmmac)
 * node can reference it via nvmem-cells = <&mac_address>.
 *
 * Gated behind CONFIG_HALLON_ROCKCHIP to keep the tree upstream-clean.
 */

#include <linux/blkdev.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* IDB (loader) starts at LBA 64; the SN sector is IDB sector 3. */
#define IDB_BASE_SECTOR		64
#define IDB_SN_SECTOR		(IDB_BASE_SECTOR + 3)
#define IDB_SN_SECTOR_SIZE	512
#define IDB_MAC_OFFSET		506
#define IDB_MAC_SIZE		6

/* The eMMC is pinned to mmcblk0 by the "mmc0 = &emmc" alias in the DTS, so
 * its whole-disk block device is MMC_BLOCK_MAJOR minor 0.  Opening by dev_t
 * (rather than a /dev path) works before devtmpfs is mounted. */
#define EMMC_BLKDEV		MKDEV(MMC_BLOCK_MAJOR, 0)

struct rockchip_idb_mac {
	u8 mac[IDB_MAC_SIZE];
};

/*
 * Rockchip IDB RC4 (P_RC4 from u-boot's board/rockchip/common/platform/
 * rc4_enc.c).  Sectors 2 and 3 of the IDB are encrypted with this fixed key.
 */
static void rockchip_idb_rc4(u8 *buf, size_t len)
{
	static const u8 key[16] = {
		124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17
	};
	u8 S[256], K[256], temp;
	unsigned int i, j, t, x;

	j = 0;
	for (i = 0; i < 256; i++) {
		S[i] = (u8)i;
		j &= 0x0f;
		K[i] = key[j];
		j++;
	}

	j = 0;
	for (i = 0; i < 256; i++) {
		j = (j + S[i] + K[i]) & 0xff;
		temp = S[i];
		S[i] = S[j];
		S[j] = temp;
	}

	i = j = 0;
	for (x = 0; x < len; x++) {
		i = (i + 1) & 0xff;
		j = (j + S[i]) & 0xff;
		temp = S[i];
		S[i] = S[j];
		S[j] = temp;
		t = (S[i] + S[j]) & 0xff;
		buf[x] ^= S[t];
	}
}

static int rockchip_idb_mac_read(void *context, unsigned int offset,
				 void *val, size_t bytes)
{
	struct rockchip_idb_mac *idb = context;

	if (offset + bytes > IDB_MAC_SIZE)
		return -EINVAL;

	memcpy(val, idb->mac + offset, bytes);

	return 0;
}

static struct nvmem_config idb_mac_nvmem_config = {
	.name = "rockchip-idb-mac",
	.type = NVMEM_TYPE_OTP,
	.read_only = true,
	.stride = 1,
	.word_size = 1,
	.size = IDB_MAC_SIZE,
	.add_legacy_fixed_of_cells = true,
};

static int rockchip_idb_mac_probe(struct platform_device *pdev)
{
	struct rockchip_idb_mac *idb;
	struct nvmem_device *nvmem;
	struct file *bdev_file;
	u8 sector[IDB_SN_SECTOR_SIZE];
	loff_t pos;
	ssize_t ret;

	idb = devm_kzalloc(&pdev->dev, sizeof(*idb), GFP_KERNEL);
	if (!idb)
		return -ENOMEM;

	bdev_file = bdev_file_open_by_dev(EMMC_BLKDEV, BLK_OPEN_READ,
					   NULL, NULL);
	if (IS_ERR(bdev_file)) {
		/* The eMMC block device appears once the MMC driver probes (the
		 * whole-disk devt is only resolvable then; before that it is
		 * -ENXIO).  Defer on any error so probe is retried once the MMC
		 * block device is registered. */
		dev_dbg(&pdev->dev, "eMMC block device not ready yet: %pe\n",
			bdev_file);
		return -EPROBE_DEFER;
	}

	pos = (loff_t)IDB_SN_SECTOR * IDB_SN_SECTOR_SIZE;
	ret = kernel_read(bdev_file, sector, sizeof(sector), &pos);
	fput(bdev_file);
	if (ret == sizeof(sector)) {
		rockchip_idb_rc4(sector, sizeof(sector));
		memcpy(idb->mac, sector + IDB_MAC_OFFSET, IDB_MAC_SIZE);
	} else {
		/* Register anyway (idb->mac stays all-zero) so the consumer
		 * falls back to a random address instead of deferring forever. */
		dev_err(&pdev->dev, "failed to read IDB SN sector: %zd\n", ret);
	}

	if (!is_valid_ether_addr(idb->mac))
		dev_warn(&pdev->dev, "invalid MAC in IDB (%pM), using random\n",
			 idb->mac);

	idb_mac_nvmem_config.dev = &pdev->dev;
	idb_mac_nvmem_config.priv = idb;
	idb_mac_nvmem_config.reg_read = rockchip_idb_mac_read;

	nvmem = devm_nvmem_register(&pdev->dev, &idb_mac_nvmem_config);
	if (IS_ERR(nvmem)) {
		dev_err(&pdev->dev, "failed to register nvmem: %pe\n", nvmem);
		return PTR_ERR(nvmem);
	}

	dev_info(&pdev->dev, "read MAC address from IDB: %pM\n", idb->mac);

	return 0;
}

static const struct of_device_id rockchip_idb_mac_match[] = {
	{ .compatible = "rockchip,rk3288-idb-mac", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_idb_mac_match);

static struct platform_driver rockchip_idb_mac_driver = {
	.probe = rockchip_idb_mac_probe,
	.driver = {
		.name = "rockchip-idb-mac",
		.of_match_table = rockchip_idb_mac_match,
	},
};
module_platform_driver(rockchip_idb_mac_driver);

MODULE_DESCRIPTION("Rockchip RK3288 IDB MAC address nvmem driver");
MODULE_LICENSE("GPL");
