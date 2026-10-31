// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner A523 CSI capture backend
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/reset.h>

#include <media/videobuf2-dma-contig.h>

#include "sun6i_csi.h"

#include "sun6i_csi_bridge.h"
#define SUN55I_A523_CSI_CCU_MODE			0x000
#define SUN55I_A523_CSI_CCU_MCSI_CLK_MODE		BIT(0)
#define SUN55I_A523_CSI_CCU_MCSI_POST_CLK_MODE	BIT(1)
#define SUN55I_A523_CSI_CCU_CLK_GATING_DISABLE	BIT(31)

#define SUN55I_A523_CSI_CCU_PARSER_CLK_EN		0x004
#define SUN55I_A523_CSI_CCU_MCSI_COMBO0_CLK_EN	BIT(8)
#define SUN55I_A523_CSI_CCU_MCSI_COMBO1_CLK_EN	BIT(9)

#define SUN55I_A523_CSI_CCU_ISP_CLK_EN		0x008
#define SUN55I_A523_CSI_CCU_MISP_BRIDGE_GATING_DISABLE	BIT(20)

#define SUN55I_A523_CSI_CCU_POST0_CLK_EN		0x00c
#define SUN55I_A523_CSI_CCU_POST1_CLK_EN		0x010
#define SUN55I_A523_CSI_CCU_MCSI_POST_CLK_EN	BIT(16)

#define SUN55I_A523_CSI_DMA_CH0_INT_EN		0x250
#define SUN55I_A523_CSI_DMA_CH0_INT_STA		0x254

#define SUN55I_A523_CSI_DMA_INT_FRAME_DONE      BIT(1)
#define SUN55I_A523_CSI_DMA_INT_VSYNC_TRIG	BIT(7)
#define SUN55I_A523_CSI_DMA_INT_VIDEO		0x002f00c7
#define SUN55I_A523_CSI_DMA_INT_ALL		0xffffffff

struct sun55i_a523_csi {
	void __iomem		*ccu_base;
	void __iomem		*top_base;
	void __iomem		*parser_base;
	void __iomem		*dma_base;

	struct clk		*clock_bus;
	struct clk		*clock_top;
	struct clk		*clock_mbus;
	struct reset_control	*reset;
};

static irqreturn_t
sun55i_a523_csi_interrupt(int irq, void *private)
{
	struct sun6i_csi_device *csi_dev = private;
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 status;

	status = readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_STA);
	if (!status)
		return IRQ_NONE;

	if (status & SUN55I_A523_CSI_DMA_INT_FRAME_DONE)
		sun6i_csi_capture_frame_done(csi_dev);

	if (status & SUN55I_A523_CSI_DMA_INT_VSYNC_TRIG)
		sun6i_csi_capture_sync(csi_dev);

	writel(status, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_STA);
	readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_STA);

	return IRQ_HANDLED;
}

static void sun55i_a523_csi_irq_enable(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 value;

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_EN);
	value |= SUN55I_A523_CSI_DMA_INT_VIDEO;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_EN);
}

static void sun55i_a523_csi_irq_disable(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 value;

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_EN);
	value &= ~SUN55I_A523_CSI_DMA_INT_ALL;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_EN);
}

static void sun55i_a523_csi_irq_clear(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;

	writel(SUN55I_A523_CSI_DMA_INT_ALL,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_STA);
	readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_INT_STA);
}

static int
sun55i_a523_csi_resources_setup(struct sun6i_csi_device *csi_dev,
				struct platform_device *platform_dev)
{
	struct sun55i_a523_csi *a523;
	struct device *dev = csi_dev->dev;
	int ret;
	int irq;

	a523 = devm_kzalloc(dev, sizeof(*a523), GFP_KERNEL);
	if (!a523)
		return -ENOMEM;

	a523->top_base =
		devm_platform_ioremap_resource_byname(platform_dev, "top");
	if (IS_ERR(a523->top_base))
		return PTR_ERR(a523->top_base);

	a523->parser_base =
		devm_platform_ioremap_resource_byname(platform_dev, "parser");
	if (IS_ERR(a523->parser_base))
		return PTR_ERR(a523->parser_base);

	a523->dma_base =
		devm_platform_ioremap_resource_byname(platform_dev, "dma");
	if (IS_ERR(a523->dma_base))
		return PTR_ERR(a523->dma_base);

	a523->ccu_base =
		devm_platform_ioremap_resource_byname(platform_dev, "ccu");
	if (IS_ERR(a523->ccu_base))
		return PTR_ERR(a523->ccu_base);

	a523->clock_bus = devm_clk_get(dev, "bus");
	if (IS_ERR(a523->clock_bus))
		return dev_err_probe(dev, PTR_ERR(a523->clock_bus),
				     "failed to acquire bus clock\n");

	a523->clock_top = devm_clk_get(dev, "top");
	if (IS_ERR(a523->clock_top))
		return dev_err_probe(dev, PTR_ERR(a523->clock_top),
				     "failed to acquire top clock\n");

	a523->clock_mbus = devm_clk_get(dev, "mbus");
	if (IS_ERR(a523->clock_mbus))
		return dev_err_probe(dev, PTR_ERR(a523->clock_mbus),
				     "failed to acquire MBUS clock\n");

	a523->reset = devm_reset_control_get_shared(dev, NULL);
	if (IS_ERR(a523->reset))
		return dev_err_probe(dev, PTR_ERR(a523->reset),
				     "failed to acquire reset\n");

	csi_dev->priv = a523;

	irq = platform_get_irq(platform_dev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, sun55i_a523_csi_interrupt, 0,
			       dev_name(dev), csi_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request DMA IRQ\n");

	return 0;
}

static void
sun55i_a523_csi_resources_cleanup(struct sun6i_csi_device *csi_dev)
{
}

static int
sun55i_a523_csi_runtime_resume(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	struct device *dev = csi_dev->dev;
	u32 value;
	int ret;

	ret = reset_control_deassert(a523->reset);
	if (ret) {
		dev_err(dev, "failed to deassert CSI reset\n");
		return ret;
	}

	ret = clk_prepare_enable(a523->clock_bus);
	if (ret) {
		dev_err(dev, "failed to enable CSI bus clock\n");
		goto error_reset;
	}

	ret = clk_prepare_enable(a523->clock_mbus);
	if (ret) {
		dev_err(dev, "failed to enable CSI MBUS clock\n");
		goto error_clock_bus;
	}

	ret = clk_prepare_enable(a523->clock_top);
	if (ret) {
		dev_err(dev, "failed to enable CSI top clock\n");
		goto error_clock_mbus;
	}

	/*
	 * Match the SUN55IW3 vendor VIN CCU bring-up:
	 * clocks -> 100 us delay -> CCU mode/gates -> combo clocks.
	 */
	usleep_range(100, 120);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);
	value |= SUN55I_A523_CSI_CCU_CLK_GATING_DISABLE |
		 SUN55I_A523_CSI_CCU_MCSI_CLK_MODE |
		 SUN55I_A523_CSI_CCU_MCSI_POST_CLK_MODE;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);
	readl(a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_ISP_CLK_EN);
	value &= ~SUN55I_A523_CSI_CCU_MISP_BRIDGE_GATING_DISABLE;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_ISP_CLK_EN);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_POST0_CLK_EN);
	value |= SUN55I_A523_CSI_CCU_MCSI_POST_CLK_EN;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_POST0_CLK_EN);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_POST1_CLK_EN);
	value |= SUN55I_A523_CSI_CCU_MCSI_POST_CLK_EN;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_POST1_CLK_EN);

	/*
	 * SUN55IW3 VIN enables COMBO0 globally. MIPI2 additionally maps to
	 * combo selector 2 / 2 == 1, therefore enable COMBO1 as well.
	 */
	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_PARSER_CLK_EN);
	value |= SUN55I_A523_CSI_CCU_MCSI_COMBO0_CLK_EN |
		 SUN55I_A523_CSI_CCU_MCSI_COMBO1_CLK_EN;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_PARSER_CLK_EN);
	readl(a523->ccu_base + SUN55I_A523_CSI_CCU_PARSER_CLK_EN);

	return 0;

error_clock_mbus:
	clk_disable_unprepare(a523->clock_mbus);

error_clock_bus:
	clk_disable_unprepare(a523->clock_bus);

error_reset:
	reset_control_assert(a523->reset);

	return ret;
}

static void
sun55i_a523_csi_runtime_suspend(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 value;

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_PARSER_CLK_EN);
	value &= ~(SUN55I_A523_CSI_CCU_MCSI_COMBO0_CLK_EN |
		   SUN55I_A523_CSI_CCU_MCSI_COMBO1_CLK_EN);
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_PARSER_CLK_EN);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_POST1_CLK_EN);
	value &= ~SUN55I_A523_CSI_CCU_MCSI_POST_CLK_EN;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_POST1_CLK_EN);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_POST0_CLK_EN);
	value &= ~SUN55I_A523_CSI_CCU_MCSI_POST_CLK_EN;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_POST0_CLK_EN);

	value = readl(a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);
	value &= ~(SUN55I_A523_CSI_CCU_MCSI_CLK_MODE |
		   SUN55I_A523_CSI_CCU_MCSI_POST_CLK_MODE);
	value |= SUN55I_A523_CSI_CCU_CLK_GATING_DISABLE;
	writel(value, a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);
	readl(a523->ccu_base + SUN55I_A523_CSI_CCU_MODE);

	clk_disable_unprepare(a523->clock_top);
	clk_disable_unprepare(a523->clock_mbus);
	clk_disable_unprepare(a523->clock_bus);
	reset_control_assert(a523->reset);
}

#define SUN55I_A523_CSI_DMA_CH0_CFG		0x204
#define SUN55I_A523_CSI_DMA_CH0_HSIZE		0x210
#define SUN55I_A523_CSI_DMA_CH0_VSIZE		0x214
#define SUN55I_A523_CSI_DMA_CH0_BUF_LEN	0x238
#define SUN55I_A523_CSI_DMA_CH0_F0_BUFA		0x220
#define SUN55I_A523_CSI_DMA_CH0_FLIP_SIZE	0x23c

#define SUN55I_A523_CSI_DMA_CFG_FIELD_EITHER	(2 << 10)
#define SUN55I_A523_CSI_DMA_CFG_RAW10		(1 << 16)

static void
sun55i_a523_csi_capture_configure(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	unsigned int width, height;
	u32 pixelformat;
	u32 stride;
	u32 value;

	sun6i_csi_capture_dimensions(csi_dev, &width, &height);
	sun6i_csi_capture_format(csi_dev, &pixelformat, NULL);

	switch (pixelformat) {
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		break;
	default:
		dev_warn(csi_dev->dev,
			 "unsupported A523 capture format: %p4cc\n",
			 &pixelformat);
		return;
	}

	stride = width * 2;

	value = SUN55I_A523_CSI_DMA_CFG_FIELD_EITHER |
		SUN55I_A523_CSI_DMA_CFG_RAW10;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_CFG);

	writel(width << 16,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_HSIZE);
	writel(height << 16,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_VSIZE);

	writel((stride << 16) | stride,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_BUF_LEN);
	writel((height << 16) | width,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_FLIP_SIZE);
}

static void
sun55i_a523_csi_buffer_configure(struct sun6i_csi_device *csi_dev,
				 struct sun6i_csi_buffer *csi_buffer)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	struct vb2_buffer *vb2_buffer = &csi_buffer->v4l2_buffer.vb2_buf;
	dma_addr_t address;

	address = vb2_dma_contig_plane_dma_addr(vb2_buffer, 0);

	writel(address >> 2,
	       a523->dma_base + SUN55I_A523_CSI_DMA_CH0_F0_BUFA);
}

#define SUN55I_A523_CSI_TOP_ISP0_IN0		0x030
#define SUN55I_A523_CSI_TOP_VIPP0_IN		0x0a0

#define SUN55I_A523_CSI_PRS_EN			0x000
#define SUN55I_A523_CSI_PRS_MCSI_IF_CFG	0x008
#define SUN55I_A523_CSI_PRS_CH0_INFMT		0x024
#define SUN55I_A523_CSI_PRS_CH0_HSIZE		0x028
#define SUN55I_A523_CSI_PRS_CH0_VSIZE		0x02c

#define SUN55I_A523_CSI_TOP_PARSER2_CH0	8

#define SUN55I_A523_CSI_DMA_TOP_EN		0x000
#define SUN55I_A523_CSI_DMA_CH0_EN		0x200
#define SUN55I_A523_CSI_DMA_EN			BIT(0)

#define SUN55I_A523_CSI_PRS_CAP			0x00c
#define SUN55I_A523_CSI_PRS_EN_MCSI		0x80008003
#define SUN55I_A523_CSI_PRS_CAP_CH0_VCAP	BIT(1)

static void
sun55i_a523_csi_bridge_configure(struct sun6i_csi_device *csi_dev,
				 struct sun6i_csi_bridge_source *source)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	unsigned int width, height;
	u32 mbus_code;

	if (source != &csi_dev->bridge.source_mipi_csi2) {
		dev_warn(csi_dev->dev, "A523 CSI only supports MIPI CSI-2 input\n");
		return;
	}

	sun6i_csi_bridge_dimensions(csi_dev, &width, &height);
	sun6i_csi_bridge_format(csi_dev, &mbus_code, NULL);
	switch (mbus_code) {
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		break;
	default:
		dev_warn(csi_dev->dev,
			 "unsupported A523 media bus format: 0x%04x\n",
			 mbus_code);
		return;
	}

	/* CSI2 parser channel 0 -> ISP0 input 0 -> VIPP0. */
	writel(SUN55I_A523_CSI_TOP_PARSER2_CH0,
	       a523->top_base + SUN55I_A523_CSI_TOP_ISP0_IN0);
	writel(0, a523->top_base + SUN55I_A523_CSI_TOP_VIPP0_IN);

	/* Progressive RAW input on MCSI channel 0. */
	writel(0, a523->parser_base + SUN55I_A523_CSI_PRS_MCSI_IF_CFG);
	writel(0, a523->parser_base + SUN55I_A523_CSI_PRS_CH0_INFMT);
	writel(width << 16,
	       a523->parser_base + SUN55I_A523_CSI_PRS_CH0_HSIZE);
	writel(height << 16,
	       a523->parser_base + SUN55I_A523_CSI_PRS_CH0_VSIZE);
}

static void sun55i_a523_csi_bridge_enable(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 value;

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_TOP_EN);
	value |= SUN55I_A523_CSI_DMA_EN;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_TOP_EN);

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_EN);
	value |= SUN55I_A523_CSI_DMA_EN;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_EN);

	writel(SUN55I_A523_CSI_PRS_EN_MCSI,
	       a523->parser_base + SUN55I_A523_CSI_PRS_EN);
	writel(SUN55I_A523_CSI_PRS_CAP_CH0_VCAP,
	       a523->parser_base + SUN55I_A523_CSI_PRS_CAP);
}

static void sun55i_a523_csi_bridge_disable(struct sun6i_csi_device *csi_dev)
{
	struct sun55i_a523_csi *a523 = csi_dev->priv;
	u32 value;

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_CH0_EN);
	value &= ~SUN55I_A523_CSI_DMA_EN;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_CH0_EN);

	value = readl(a523->dma_base + SUN55I_A523_CSI_DMA_TOP_EN);
	value &= ~SUN55I_A523_CSI_DMA_EN;
	writel(value, a523->dma_base + SUN55I_A523_CSI_DMA_TOP_EN);

	writel(0, a523->parser_base + SUN55I_A523_CSI_PRS_CAP);
	writel(0, a523->parser_base + SUN55I_A523_CSI_PRS_EN);
}

static const struct sun6i_csi_hw_ops sun55i_a523_csi_hw_ops = {
	.resources_setup	= sun55i_a523_csi_resources_setup,
	.resources_cleanup	= sun55i_a523_csi_resources_cleanup,
	.runtime_resume		= sun55i_a523_csi_runtime_resume,
	.runtime_suspend	= sun55i_a523_csi_runtime_suspend,
	.irq_enable		= sun55i_a523_csi_irq_enable,
	.irq_disable		= sun55i_a523_csi_irq_disable,
	.irq_clear		= sun55i_a523_csi_irq_clear,
	.bridge_enable		= sun55i_a523_csi_bridge_enable,
	.bridge_disable		= sun55i_a523_csi_bridge_disable,
	.bridge_configure	= sun55i_a523_csi_bridge_configure,
	.capture_configure	= sun55i_a523_csi_capture_configure,
	.buffer_configure	= sun55i_a523_csi_buffer_configure,
};

const struct sun6i_csi_variant sun55i_a523_csi_variant = {
	.hw_ops			= &sun55i_a523_csi_hw_ops,
	.source_stream_last	= true,
};
