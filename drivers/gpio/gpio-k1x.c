// SPDX-License-Identifier: GPL-2.0
/*
 * spacemit-k1x gpio driver file
 *
 * Copyright (C) 2023 Spacemit
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/irqdomain.h>

#define GPLR		0x0
#define GPDR		0xc
#define GPSR		0x18
#define GPCR		0x24
#define GRER		0x30
#define GFER		0x3c
#define GEDR		0x48
#define GSDR		0x54
#define GCDR		0x60
#define GSRER		0x6c
#define GCRER		0x78
#define GSFER		0x84
#define GCFER		0x90
#define GAPMASK		0x9c
#define GCPMASK		0xa8

#define K1X_BANK_GPIO_NUMBER	(32)
#define BANK_GPIO_MASK		(K1X_BANK_GPIO_NUMBER - 1)

#define k1x_gpio_to_bank_idx(gpio)	((gpio)/K1X_BANK_GPIO_NUMBER)
#define k1x_gpio_to_bank_offset(gpio)	((gpio) & BANK_GPIO_MASK)
#define k1x_bank_to_gpio(idx, offset)	(((idx) * K1X_BANK_GPIO_NUMBER)	\
						| ((offset) & BANK_GPIO_MASK))

struct k1x_gpio_bank {
	void __iomem *reg_bank;
	u32 irq_mask;
	u32 irq_rising_edge;
	u32 irq_falling_edge;
};

struct k1x_gpio_chip {
	struct gpio_chip chip;
	void __iomem *reg_base;
	int irq;
	struct irq_domain *domain;
	unsigned int ngpio;
	unsigned int nbank;
	struct k1x_gpio_bank *banks;
};

static int k1x_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);

	return irq_create_mapping(k1x_chip->domain, offset);
}

static int k1x_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(offset)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(offset));

	writel(bit, bank->reg_bank + GCDR);

	return 0;
}

static int k1x_gpio_direction_output(struct gpio_chip *chip,
				     unsigned offset, int value)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(offset)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(offset));

	/* Set value first. */
	writel(bit, bank->reg_bank + (value ? GPSR : GPCR));

	writel(bit, bank->reg_bank + GSDR);

	return 0;
}

static int k1x_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(offset)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(offset));
	u32 gplr;

	gplr  = readl(bank->reg_bank + GPLR);

	return !!(gplr & bit);
}

static void k1x_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(offset)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(offset));
	u32 gpdr;

	gpdr = readl(bank->reg_bank + GPDR);
	/* Is it configured as output? */
	if (gpdr & bit)
		writel(bit, bank->reg_bank + (value ? GPSR : GPCR));
}

#ifdef CONFIG_OF_GPIO
static int k1x_gpio_of_xlate(struct gpio_chip *chip,
			     const struct of_phandle_args *gpiospec,
			     u32 *flags)
{
	struct k1x_gpio_chip *k1x_chip =
			container_of(chip, struct k1x_gpio_chip, chip);

	/* GPIO index start from 0. */
	if (gpiospec->args[0] >= k1x_chip->ngpio)
		return -EINVAL;

	if (flags)
		*flags = gpiospec->args[1];

	return gpiospec->args[0];
}
#endif

static int k1x_gpio_irq_type(struct irq_data *d, unsigned int type)
{
	struct k1x_gpio_chip *k1x_chip = irq_data_get_irq_chip_data(d);
	int gpio = irqd_to_hwirq(d);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(gpio)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(gpio));

	if (type & IRQ_TYPE_EDGE_RISING) {
		bank->irq_rising_edge |= bit;
		writel(bit, bank->reg_bank + GSRER);
	} else {
		bank->irq_rising_edge &= ~bit;
		writel(bit, bank->reg_bank + GCRER);
	}

	if (type & IRQ_TYPE_EDGE_FALLING) {
		bank->irq_falling_edge |= bit;
		writel(bit, bank->reg_bank + GSFER);
	} else {
		bank->irq_falling_edge &= ~bit;
		writel(bit, bank->reg_bank + GCFER);
	}

	return 0;
}

static irqreturn_t k1x_gpio_demux_handler(int irq, void *data)
{
	struct k1x_gpio_chip *k1x_chip = (struct k1x_gpio_chip *)data;
	struct k1x_gpio_bank *bank;
	int i, n;
	u32 gedr;
	unsigned long pending = 0;
	unsigned int irqs_handled = 0;

	for (i = 0; i < k1x_chip->nbank; i++) {
		bank = &k1x_chip->banks[i];

		gedr = readl(bank->reg_bank + GEDR);
		if (!gedr)
			continue;

		writel(gedr, bank->reg_bank + GEDR);
		gedr = gedr & bank->irq_mask;

		if (!gedr)
			continue;
		pending = gedr;
		for_each_set_bit(n, &pending, BITS_PER_LONG) {
			generic_handle_irq(irq_find_mapping(k1x_chip->domain,
						k1x_bank_to_gpio(i, n)));
		}
		irqs_handled++;
	}

	return irqs_handled ? IRQ_HANDLED : IRQ_NONE;
}

static void k1x_ack_muxed_gpio(struct irq_data *d)
{
	struct k1x_gpio_chip *k1x_chip = irq_data_get_irq_chip_data(d);
	int gpio = irqd_to_hwirq(d);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(gpio)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(gpio));

	writel(bit, bank->reg_bank + GEDR);
}

static void k1x_mask_muxed_gpio(struct irq_data *d)
{
	struct k1x_gpio_chip *k1x_chip = irq_data_get_irq_chip_data(d);
	int gpio = irqd_to_hwirq(d);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(gpio)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(gpio));

	bank->irq_mask &= ~bit;

	/* Clear the bit of rising and falling edge detection. */
	writel(bit, bank->reg_bank + GCRER);
	writel(bit, bank->reg_bank + GCFER);
}

static void k1x_unmask_muxed_gpio(struct irq_data *d)
{
	struct k1x_gpio_chip *k1x_chip = irq_data_get_irq_chip_data(d);
	int gpio = irqd_to_hwirq(d);
	struct k1x_gpio_bank *bank =
			&k1x_chip->banks[k1x_gpio_to_bank_idx(gpio)];
	u32 bit = (1 << k1x_gpio_to_bank_offset(gpio));

	bank->irq_mask |= bit;

	/* Set the bit of rising and falling edge detection if the gpio has. */
	writel(bit & bank->irq_rising_edge, bank->reg_bank + GSRER);
	writel(bit & bank->irq_falling_edge, bank->reg_bank + GSFER);
}

static struct irq_chip k1x_muxed_gpio_chip = {
	.name		= "k1x-gpio-irqchip",
	.irq_ack	= k1x_ack_muxed_gpio,
	.irq_mask	= k1x_mask_muxed_gpio,
	.irq_unmask	= k1x_unmask_muxed_gpio,
	.irq_set_type	= k1x_gpio_irq_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

static const struct of_device_id k1x_gpio_dt_ids[] = {
	{ .compatible = "spacemit,k1x-gpio"},
	{}
};

static int k1x_irq_domain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &k1x_muxed_gpio_chip,
				 handle_edge_irq);
	irq_set_chip_data(irq, d->host_data);

	return 0;
}

static const struct irq_domain_ops k1x_gpio_irq_domain_ops = {
	.map	= k1x_irq_domain_map,
	.xlate	= irq_domain_xlate_twocell,
};

static int k1x_gpio_probe_dt(struct platform_device *pdev,
				struct k1x_gpio_chip *k1x_chip)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	u32 offset;
	int i, nbank, ret;

	nbank = of_get_child_count(np);
	if (nbank == 0)
		return -EINVAL;

	k1x_chip->banks = devm_kzalloc(&pdev->dev,
				sizeof(*k1x_chip->banks) * nbank,
				GFP_KERNEL);
	if (k1x_chip->banks == NULL)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node(np, child) {
		ret = of_property_read_u32(child, "reg-offset", &offset);
		if (ret) {
			of_node_put(child);
			return ret;
		}
		k1x_chip->banks[i].reg_bank = k1x_chip->reg_base + offset;
		i++;
	}

	k1x_chip->nbank = nbank;
	k1x_chip->ngpio = k1x_chip->nbank * K1X_BANK_GPIO_NUMBER;

	return 0;
}

static int k1x_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct k1x_gpio_chip *k1x_chip;
	struct k1x_gpio_bank *bank;
	struct resource *res;
	struct irq_domain *domain;
	struct clk *clk;

	int irq, i, ret;
	void __iomem *base;

	np = pdev->dev.of_node;
	if (!np)
		return -EINVAL;

	k1x_chip = devm_kzalloc(dev, sizeof(*k1x_chip), GFP_KERNEL);
	if (k1x_chip == NULL)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	base = devm_ioremap_resource(dev, res);
	if (!base)
		return -EINVAL;

	k1x_chip->irq = irq;
	k1x_chip->reg_base = base;

	ret = k1x_gpio_probe_dt(pdev, k1x_chip);
	if (ret) {
		dev_err(dev, "Fail to initialize gpio unit, error %d.\n", ret);
		return ret;
	}

	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "Fail to get gpio clock, error %ld.\n",
			PTR_ERR(clk));
		return PTR_ERR(clk);
	}
	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(dev, "Fail to enable gpio clock, error %d.\n", ret);
		return ret;
	}

	domain = irq_domain_add_linear(np, k1x_chip->ngpio,
					&k1x_gpio_irq_domain_ops, k1x_chip);
	if (domain == NULL)
		return -EINVAL;

	k1x_chip->domain = domain;

	/* Initialize the gpio chip */
	k1x_chip->chip.label = "k1x-gpio";
	k1x_chip->chip.request = gpiochip_generic_request;
	k1x_chip->chip.free = gpiochip_generic_free;
	k1x_chip->chip.direction_input  = k1x_gpio_direction_input;
	k1x_chip->chip.direction_output = k1x_gpio_direction_output;
	k1x_chip->chip.get = k1x_gpio_get;
	k1x_chip->chip.set = k1x_gpio_set;
	k1x_chip->chip.to_irq = k1x_gpio_to_irq;
#ifdef CONFIG_OF_GPIO
	k1x_chip->chip.of_node = np;
	k1x_chip->chip.of_xlate = k1x_gpio_of_xlate;
	k1x_chip->chip.of_gpio_n_cells = 2;
#endif
	k1x_chip->chip.ngpio = k1x_chip->ngpio;

	if (devm_request_irq(&pdev->dev, irq,
			     k1x_gpio_demux_handler, 0, k1x_chip->chip.label, k1x_chip)) {
		dev_err(&pdev->dev, "failed to request high IRQ\n");
		ret = -ENOENT;
		goto err;
	}

	gpiochip_add(&k1x_chip->chip);

	/* clear all GPIO edge detects */
	for (i = 0; i < k1x_chip->nbank; i++) {
		bank = &k1x_chip->banks[i];
		writel(0xffffffff, bank->reg_bank + GCFER);
		writel(0xffffffff, bank->reg_bank + GCRER);
		/* Unmask edge detection to AP. */
		writel(0xffffffff, bank->reg_bank + GAPMASK);
	}

	return 0;
err:
	irq_domain_remove(domain);
	return ret;
}

static struct platform_driver k1x_gpio_driver = {
	.probe		= k1x_gpio_probe,
	.driver		= {
		.name	= "k1x-gpio",
		.of_match_table = k1x_gpio_dt_ids,
	},
};

static int __init k1x_gpio_init(void)
{
	return platform_driver_register(&k1x_gpio_driver);
}
subsys_initcall(k1x_gpio_init);
