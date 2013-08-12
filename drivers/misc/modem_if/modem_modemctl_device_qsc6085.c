/* /linux/drivers/misc/modem_if/modem_modemctl_device_QSC7.1.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>

#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/platform_device.h>

#include <linux/platform_data/modem.h>
#include "modem_prj.h"
#include "modem_link_device_idpram.h"

static irqreturn_t phone_active_handler(int irq, void *arg)
{
	struct modem_ctl *mc = (struct modem_ctl *)arg;
	int phone_reset = gpio_get_value(mc->gpio_cp_reset);
	int phone_active = gpio_get_value(mc->gpio_phone_active);
	int phone_state = mc->phone_state;

	pr_info("[QSC] <%s> state = %d, phone_reset = %d, phone_active = %d\n",
		__func__, phone_state, phone_reset, phone_active);

	if (phone_reset && phone_active) {
		phone_state = STATE_ONLINE;
		mc->iod->modem_state_changed(mc->iod, phone_state);
	} else if (phone_reset && !phone_active) {
		if (mc->phone_state == STATE_ONLINE) {
			phone_state = STATE_CRASH_EXIT;
			mc->iod->modem_state_changed(mc->iod, phone_state);
		}
	} else {
		phone_state = STATE_OFFLINE;
		if (mc->iod && mc->iod->modem_state_changed)
			mc->iod->modem_state_changed(mc->iod, phone_state);
	}

	if (phone_active)
		irq_set_irq_type(mc->irq_phone_active, IRQ_TYPE_LEVEL_LOW);
	else
		irq_set_irq_type(mc->irq_phone_active, IRQ_TYPE_LEVEL_HIGH);

	pr_info("[QSC] <%s> phone_state = %d\n", __func__, phone_state);

	return IRQ_HANDLED;
}

static int qsc6085_on(struct modem_ctl *mc)
{
	struct dpram_link_device *dpld = to_dpram_link_device(mc->iod->link);

	pr_info("[QSC] <%s> start!!!\n", __func__);

	if (!mc->gpio_cp_on || !mc->gpio_cp_reset) {
		pr_err("[MODEM_IF] no gpio data\n");
		return -ENXIO;
	}

	dpld->init_magic_num(dpld);

	gpio_set_value(mc->gpio_cp_reset, 1);
	gpio_set_value(mc->gpio_cp_on, 0);

	msleep(100);

	gpio_set_value(mc->gpio_cp_on, 1);

	msleep(400);
	msleep(400);
	msleep(200);

	gpio_set_value(mc->gpio_cp_on, 0);

	mc->iod->modem_state_changed(mc->iod, STATE_BOOTING);

	return 0;
}

static int qsc6085_off(struct modem_ctl *mc)
{
	int phone_wait_cnt = 0;

	pr_info("[QSC] qsc6085_off()\n");

	if (!mc->gpio_cp_off || !mc->gpio_cp_reset) {
		pr_err("[QSC] no gpio data\n");
		return -ENXIO;
	}

	gpio_set_value(mc->gpio_cp_on, 0);

	/*  confirm phone off */
	while (1) {
		if (gpio_get_value(mc->gpio_phone_active)) {
			pr_err("[QSC] <%s>Try to Turn Phone Off by CP_RST\n",
				__func__);
			gpio_set_value(mc->gpio_cp_reset, 0);
			if (phone_wait_cnt > 10) {
				pr_emerg("[QSC]<%s>: OFF Failed\n", __func__);
				break;
			}
			phone_wait_cnt++;
			mdelay(100);
		} else {
			pr_emerg("[QSC] <%s>: OFF Success\n", __func__);
			break;
		}
	}


	mc->iod->modem_state_changed(mc->iod, STATE_OFFLINE);

	return 0;
}

static int qsc6085_reset(struct modem_ctl *mc)
{
	struct dpram_link_device *dpld = to_dpram_link_device(mc->iod->link);

	pr_debug("[QSC] qsc6085_reset()\n");

	if (dpld->link_pm_data->idpram_wpend == (int)IDPRAM_WPEND_LOCK)
		dpld->link_pm_data->idpram_wpend == (int)IDPRAM_WPEND_UNLOCK;

	dpld->init_magic_num(dpld);

	gpio_set_value(mc->gpio_cp_reset, 0);
	msleep(100);
	gpio_set_value(mc->gpio_cp_reset, 1);

	return 0;
}

static void qsc6085_get_ops(struct modem_ctl *mc)
{
	mc->ops.modem_on = qsc6085_on;
	mc->ops.modem_off = qsc6085_off;
	mc->ops.modem_reset = qsc6085_reset;
}

int qsc6085_init_modemctl_device(struct modem_ctl *mc, struct modem_data *pdata)
{
	int ret = 0;
	int irq = 0;
	unsigned long flag = 0;
	struct platform_device *pdev = NULL;

	mc->gpio_cp_on         = pdata->gpio_cp_on;
	mc->gpio_cp_reset      = pdata->gpio_cp_reset;
	mc->gpio_pda_active    = pdata->gpio_pda_active;
	mc->gpio_phone_active  = pdata->gpio_phone_active;
	mc->gpio_host_wakeup = pdata->gpio_host_wakeup;
	mc->gpio_cp_dump_int = pdata->gpio_cp_dump_int;

	if (!mc->gpio_cp_on || !mc->gpio_cp_reset || !mc->gpio_phone_active) {
		mif_err("no GPIO data\n");
		return -ENXIO;
	}

	gpio_set_value(mc->gpio_cp_reset, 0);
	gpio_set_value(mc->gpio_cp_on, 0);

	qsc6085_get_ops(mc);

	/*register phone_active_handler*/
	pdev = to_platform_device(mc->dev);
	mc->irq_phone_active = platform_get_irq_byname(pdev,
											"phone_active_irq");
	if (!mc->irq_phone_active) {
		mif_err("get irq fail\n");
		return -1;
	}

	irq = mc->irq_phone_active;
	pr_info("[QSC] <%s> PHONE_ACTIVE IRQ# = %d\n", __func__, irq);

	flag = IRQF_TRIGGER_HIGH;
	ret = request_irq(irq,
				phone_active_handler,
				flag,
				"qsc_phone_active",
				mc);
	if (ret) {
		pr_err("[QSC] <%s> request_irq fail (%d)\n", __func__, ret);
		return ret;
	}

	ret = enable_irq_wake(irq);
	if (ret)
		pr_err("[QSC] <%s> enable_irq_wake fail (%d)\n", __func__, ret);

	return 0;
}
