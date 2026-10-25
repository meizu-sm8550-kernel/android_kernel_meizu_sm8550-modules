/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CS35L43_REGMAP_H
#define __CS35L43_REGMAP_H

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>

#define CS35L43_IO_ATTEMPTS 5

static inline void cs35l43_regmap_error(struct regmap *map, const char *op,
				      unsigned int reg, int attempt, int ret)
{
	dev_err(regmap_get_device(map), "%s %x attempt %d failed: %d\n",
		op, reg, attempt + 1, ret);
	usleep_range(1000, 2000);
}

#define CS35L43_REGMAP_RETRY(map, reg, op) ({			\
	int _attempt, _ret;					\
	for (_attempt = 0; _attempt < CS35L43_IO_ATTEMPTS; _attempt++) { \
		_ret = (op);					\
		if (!_ret)					\
			break;					\
		cs35l43_regmap_error(map, __func__, reg, _attempt, _ret); \
	}							\
	_ret;							\
})

static inline int cs35l43_reg_read(struct regmap *map, unsigned int reg,
				 unsigned int *val)
{
	return CS35L43_REGMAP_RETRY(map, reg, regmap_read(map, reg, val));
}

static inline int cs35l43_reg_write(struct regmap *map, unsigned int reg,
				  unsigned int val)
{
	return CS35L43_REGMAP_RETRY(map, reg, regmap_write(map, reg, val));
}

static inline int cs35l43_reg_update_bits(struct regmap *map, unsigned int reg,
					unsigned int mask, unsigned int val)
{
	return CS35L43_REGMAP_RETRY(map, reg,
				  regmap_update_bits(map, reg, mask, val));
}

static inline int cs35l43_raw_write(struct regmap *map, unsigned int reg,
				    const void *val, size_t len)
{
	return CS35L43_REGMAP_RETRY(map, reg,
				  regmap_raw_write(map, reg, val, len));
}

static inline int cs35l43_raw_write_async(struct regmap *map, unsigned int reg,
					  const void *val, size_t len)
{
	return CS35L43_REGMAP_RETRY(map, reg,
				  regmap_raw_write_async(map, reg, val, len));
}

static inline int cs35l43_raw_read(struct regmap *map, unsigned int reg,
				   void *val, size_t len, bool repeat)
{
	int attempt, ret;

	/* Stock coefficient and log reads always take five samples. */
	for (attempt = 0; attempt < CS35L43_IO_ATTEMPTS; attempt++) {
		ret = regmap_raw_read(map, reg, val, len);
		if (ret)
			cs35l43_regmap_error(map, __func__, reg, attempt, ret);
		else if (!repeat)
			break;
	}

	return ret;
}

#undef CS35L43_REGMAP_RETRY

#endif
