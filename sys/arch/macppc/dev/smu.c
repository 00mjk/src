/*-
 * Copyright (c) 2013 Phileas Fogg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/proc.h>
#include <sys/mutex.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>

#include <machine/autoconf.h>

#include <dev/ofw/openfirm.h>
#include <dev/i2c/i2cvar.h>
#include <dev/clock_subr.h>
#include <dev/sysmon/sysmonvar.h>
#include <dev/sysmon/sysmon_taskq.h>

#include <macppc/dev/obiovar.h>
#include <macppc/dev/smuvar.h>
#include <macppc/dev/fancontrolvar.h>

#include "opt_smu.h"

struct smu_softc;

struct smu_cmd {
	u_char cmd;
	u_char len;
	u_char data[254];
};

struct smu_fan {
	struct smu_softc* sc;

	char location[32];
	int reg;
	int zone;
	int rpm_ctl;
	int min_rpm;
	int max_rpm;
	int default_rpm;
	int wanted_rpm;
	int current_rpm;
	int fault;
	time_t last_update;
};

struct smu_iicbus {
	struct smu_softc* sc;

	int reg;
	struct i2c_controller i2c;
};

#define SMU_MAX_FANS		8
#define SMU_MAX_IICBUS		3
#define SMU_MAX_SME_SENSORS	(SMU_MAX_FANS + 8)


#define SMU_ZONE_CPU		0
#define SMU_ZONE_CASE		1
#define SMU_ZONE_DRIVEBAY	2
#define SMU_ZONES		3

#define C_TO_uK(n) (n * 1000000 + 273150000)

struct smu_softc {
	device_t sc_dev;
	int sc_node;
	struct sysctlnode *sc_sysctl_me;

	kmutex_t sc_cmd_lock;
	kmutex_t sc_msg_lock;
	struct smu_cmd *sc_cmd;
	paddr_t sc_cmd_paddr;
	int sc_dbell_mbox;
	int sc_dbell_gpio;

	int sc_num_fans;
	struct smu_fan sc_fans[SMU_MAX_FANS];

	int sc_num_iicbus;
	struct smu_iicbus sc_iicbus[SMU_MAX_IICBUS];

	struct todr_chip_handle sc_todr;

	struct sysmon_envsys *sc_sme;
	envsys_data_t sc_sme_sensors[SMU_MAX_SME_SENSORS];
	uint32_t cpu_m;
	int32_t  cpu_b;

	fancontrol_zone_t sc_zones[SMU_ZONES];
	lwp_t *sc_thread;
	bool sc_dying;
};

#define SMU_CMD_FAN	0x4a
#define SMU_CMD_RTC	0x8e
#define SMU_CMD_I2C	0x9a
#define SMU_CMD_POWER	0xaa
#define SMU_CMD_ADC	0xd8
#define SMU_MISC	0xee
#define  SMU_MISC_GET_DATA	0x02
#define  SMU_MISC_LED_CTRL	0x04

#define SMU_CPUTEMP_CAL 0x18
#define SMU_CPUVOLT_CAL	0x21
#define SMU_SLOTPW_CAL	0x78

#define SMU_PARTITION		0x3e
#define SMU_PARTITION_LATEST	0x01
#define SMU_PARTITION_BASE	0x02
#define SMU_PARTITION_UPDATE	0x03

#ifdef SMU_DEBUG
#define DPRINTF printf
#else
#define DPRINTF while (0) printf
#endif

static int smu_match(device_t, struct cfdata *, void *);
static void smu_attach(device_t, device_t, void *);
static int smu_setup_doorbell(struct smu_softc *);
static void smu_setup_fans(struct smu_softc *);
static void smu_setup_iicbus(struct smu_softc *);
static void smu_setup_sme(struct smu_softc *);
static int smu_iicbus_print(void *, const char *);
static void smu_sme_refresh(struct sysmon_envsys *, envsys_data_t *);
static int smu_do_cmd(struct smu_softc *, struct smu_cmd *, int);
static int smu_dbell_gpio_intr(void *);
static int smu_todr_gettime_ymdhms(todr_chip_handle_t, struct clock_ymdhms *);
static int smu_todr_settime_ymdhms(todr_chip_handle_t, struct clock_ymdhms *);
static int smu_fan_update_rpm(struct smu_fan *);
static int smu_read_adc(struct smu_softc *, int);

static int smu_iicbus_exec(void *, i2c_op_t, i2c_addr_t, const void *,
    size_t, void *, size_t, int);

static void smu_setup_zones(struct smu_softc *);
static void smu_adjust(void *);

static bool is_cpu_sensor(const envsys_data_t *);
static bool is_drive_sensor(const envsys_data_t *);
static bool is_slots_sensor(const envsys_data_t *);
static int smu_fan_get_rpm(void *, int);
static int smu_fan_set_rpm(void *, int, int);

int smu_get_datablock(int, uint8_t *, size_t);

CFATTACH_DECL_NEW(smu, sizeof(struct smu_softc),
    smu_match, smu_attach, NULL, NULL);

static struct smu_softc *smu0 = NULL;

static int
smu_match(device_t parent, struct cfdata *cf, void *aux)
{
	struct confargs *ca = aux;

	if (strcmp(ca->ca_name, "smu") == 0)
		return 5;
	
	return 0;
}

static void
smu_attach(device_t parent, device_t self, void *aux)
{
	struct confargs *ca = aux;
	struct smu_softc *sc = device_private(self);
	uint16_t data[4];

	sc->sc_dev = self;
	sc->sc_node = ca->ca_node;

	if (smu0 == NULL)
		smu0 = sc;

	sysctl_createv(NULL, 0, NULL, (void *) &sc->sc_sysctl_me,
	    CTLFLAG_READWRITE,
	    CTLTYPE_NODE, device_xname(sc->sc_dev), NULL,
	    NULL, 0, NULL, 0,
	    CTL_MACHDEP, CTL_CREATE, CTL_EOL);

	if (smu_setup_doorbell(sc) != 0) {
		aprint_normal(": unable to set up doorbell\n");
		return;
	}

	aprint_normal("\n");

	smu_setup_fans(sc);
	smu_setup_iicbus(sc);

	sc->sc_todr.todr_gettime_ymdhms = smu_todr_gettime_ymdhms;
	sc->sc_todr.todr_settime_ymdhms = smu_todr_settime_ymdhms;
	sc->sc_todr.cookie = sc;
	todr_attach(&sc->sc_todr);

	/* calibration data */
	memset(data, 0, 8);	
	smu_get_datablock(SMU_CPUTEMP_CAL, (void *)data, 8);
	DPRINTF("data %04x %04x %04x %04x\n", data[0], data[1], data[2], data[3]);
	sc->cpu_m = data[2]; 
	sc->cpu_b = (int16_t)data[3];

	smu_setup_sme(sc);

	smu_setup_zones(sc);
}

static int
smu_setup_doorbell(struct smu_softc *sc)
{
	int node, parent, reg[4], gpio_base, irq;

	mutex_init(&sc->sc_cmd_lock, MUTEX_DEFAULT, IPL_NONE);
	sc->sc_cmd = malloc(4096, M_DEVBUF, M_WAITOK);
	sc->sc_cmd_paddr = vtophys((vaddr_t) sc->sc_cmd);

	DPRINTF("%s: cmd vaddr 0x%x paddr 0x%x\n",
	    __func__, (unsigned int) sc->sc_cmd,
	    (unsigned int) sc->sc_cmd_paddr);

	if (OF_getprop(sc->sc_node, "platform-doorbell-buff",
	        &node, sizeof(node)) <= 0)
		return -1;

	if (OF_getprop(node, "platform-do-doorbell-buff",
	        reg, sizeof(reg)) < sizeof(reg))
		return -1;

	sc->sc_dbell_mbox = reg[3];

	if (OF_getprop(sc->sc_node, "platform-doorbell-ack",
	        &node, sizeof(node)) <= 0)
		return -1;

	parent = OF_parent(node);
	if (parent == 0)
		return -1;

	if (OF_getprop(parent, "reg", &gpio_base, sizeof(gpio_base)) <= 0)
		return -1;

	if (OF_getprop(node, "reg", reg, sizeof(reg)) <= 0)
		return -1;

	if (OF_getprop(node, "interrupts", &irq, sizeof(irq)) <= 0)
		return -1;

	sc->sc_dbell_gpio = gpio_base + reg[0];

	aprint_normal(" mbox 0x%x gpio 0x%x irq %d",
	    sc->sc_dbell_mbox, sc->sc_dbell_gpio, irq);

	intr_establish_xname(irq, IST_EDGE_FALLING, IPL_TTY,
	    smu_dbell_gpio_intr, sc, device_xname(sc->sc_dev));

	return 0;
}

static void
smu_setup_fans(struct smu_softc *sc)
{
	struct smu_fan *fan;
	char type[32];
	int node, i;
	const char *fans[] = { "fans", "rpm-fans", 0 };
	int n = 0;
	
	while (fans[n][0] != 0) {
		node = of_getnode_byname(sc->sc_node, fans[n]);
		for (node = OF_child(node);
		    (node != 0) && (sc->sc_num_fans < SMU_MAX_FANS);
		    node = OF_peer(node)) {
			fan = &sc->sc_fans[sc->sc_num_fans];
			fan->sc = sc;

			memset(fan->location, 0, sizeof(fan->location));
			OF_getprop(node, "location", fan->location,
			    sizeof(fan->location));

			if (OF_getprop(node, "reg", &fan->reg,
			        sizeof(fan->reg)) <= 0)
				continue;

			if (OF_getprop(node, "zone", &fan->zone	,
			        sizeof(fan->zone)) <= 0)
				continue;

			memset(type, 0, sizeof(type));
			OF_getprop(node, "device_type", type, sizeof(type));
			if (strcmp(type, "fan-rpm-control") == 0)
				fan->rpm_ctl = 1;
			else
				fan->rpm_ctl = 0;

			if (OF_getprop(node, "min-value", &fan->min_rpm,
			    sizeof(fan->min_rpm)) <= 0)
				fan->min_rpm = 0;

			if (OF_getprop(node, "max-value", &fan->max_rpm,
			    sizeof(fan->max_rpm)) <= 0)
				fan->max_rpm = 0xffff;

			if (OF_getprop(node, "unmanage-value", &fan->default_rpm,
			    sizeof(fan->default_rpm)) <= 0)
				fan->default_rpm = fan->max_rpm;

			DPRINTF("fan: location %s reg %x zone %d rpm_ctl %d "
			    "min_rpm %d max_rpm %d default_rpm %d\n",
			    fan->location, fan->reg, fan->zone, fan->rpm_ctl,
			    fan->min_rpm, fan->max_rpm, fan->default_rpm);

			fan->wanted_rpm = fan->default_rpm;
			fan->fault = 0; 
			sc->sc_num_fans++;
		}
		n++;
	}

	for (i = 0; i < sc->sc_num_fans; i++) {
		fan = &sc->sc_fans[i];
		smu_fan_set_rpm(sc, i, fan->default_rpm);
		smu_fan_update_rpm(fan);
	}
}

static void
smu_setup_iicbus(struct smu_softc *sc)
{
	struct smu_iicbus *iicbus;
	struct i2c_controller *i2c;
	struct smu_iicbus_confargs ca;
	int node;
	char name[32];

	node = of_getnode_byname(sc->sc_node, "smu-i2c-control");
	if (node == 0) node = sc->sc_node;
	for (node = OF_child(node);
	    (node != 0) && (sc->sc_num_iicbus < SMU_MAX_IICBUS);
	    node = OF_peer(node)) {
		memset(name, 0, sizeof(name));
		OF_getprop(node, "name", name, sizeof(name));
		if ((strcmp(name, "i2c-bus") != 0) &&
		    (strcmp(name, "i2c") != 0))
			continue;

		iicbus = &sc->sc_iicbus[sc->sc_num_iicbus];
		iicbus->sc = sc;
		i2c = &iicbus->i2c;

		if (OF_getprop(node, "reg", &iicbus->reg, sizeof(iicbus->reg)) <= 0)
			continue;

		DPRINTF("iicbus: reg %x\n", iicbus->reg);

		iic_tag_init(i2c);
		i2c->ic_cookie = iicbus;
		i2c->ic_exec = smu_iicbus_exec;

		ca.ca_name = name;
		ca.ca_node = node;
		ca.ca_tag = i2c;
		config_found(sc->sc_dev, &ca, smu_iicbus_print,
		    CFARGS(.devhandle = devhandle_from_of(node)));

		sc->sc_num_iicbus++;
	}
}

static void
smu_setup_sme(struct smu_softc *sc)
{
	struct smu_fan *fan;
	envsys_data_t *sme_sensor;
	int i, sensors, child, reg;
	char loc[32], type[32];

	sc->sc_sme = sysmon_envsys_create();

	for (i = 0; i < sc->sc_num_fans; i++) {
		sme_sensor = &sc->sc_sme_sensors[i];
		fan = &sc->sc_fans[i];

		sme_sensor->units = ENVSYS_SFANRPM;
		sme_sensor->state = ENVSYS_SINVALID;
		snprintf(sme_sensor->desc, sizeof(sme_sensor->desc),
		    "%s", fan->location);

		if (sysmon_envsys_sensor_attach(sc->sc_sme, sme_sensor)) {
			sysmon_envsys_destroy(sc->sc_sme);
			return;
		}
	}
	sensors = OF_finddevice("/smu/sensors");
	child = OF_child(sensors);
	while (child != 0) {
		sme_sensor = &sc->sc_sme_sensors[i];
		if (OF_getprop(child, "location", loc, 32) == 0) goto next;
		if (OF_getprop(child, "device_type", type, 32) == 0) goto next;
		if (OF_getprop(child, "reg", &reg, 4) == 0) goto next;
		if (strcmp(type, "temp-sensor") == 0) {
			sme_sensor->units = ENVSYS_STEMP;
			sme_sensor->state = ENVSYS_SINVALID;
			strncpy(sme_sensor->desc, loc, sizeof(sme_sensor->desc));
			sme_sensor->private = reg;
			sysmon_envsys_sensor_attach(sc->sc_sme, sme_sensor);
			i++;
			printf("%s: %s@%x\n", loc, type, reg); 
		}
next:
		child = OF_peer(child);
	}
						
	sc->sc_sme->sme_name = device_xname(sc->sc_dev);
	sc->sc_sme->sme_cookie = sc;
	sc->sc_sme->sme_refresh = smu_sme_refresh;

	if (sysmon_envsys_register(sc->sc_sme)) {
		aprint_error_dev(sc->sc_dev,
		    "unable to register with sysmon\n");
		sysmon_envsys_destroy(sc->sc_sme);
	}
}

static int
smu_iicbus_print(void *aux, const char *smu)
{
	struct smu_iicbus_confargs *ca = aux;

	if (smu)
		aprint_normal("%s at %s", ca->ca_name, smu);

	return UNCONF;
}

static void
smu_sme_refresh(struct sysmon_envsys *sme, envsys_data_t *edata)
{
	struct smu_softc *sc = sme->sme_cookie;
	int which = edata->sensor;
	int ret;

	edata->state = ENVSYS_SINVALID;

	if (which < sc->sc_num_fans) {

		ret = smu_fan_get_rpm(sc, which);
		if (ret != -1) {
			sc->sc_fans[which].current_rpm = ret;
			edata->value_cur = ret;
			edata->state = ENVSYS_SVALID;
		}
	} else if (edata->private > 0) {
		/* this works only for the CPU diode */
		int64_t r = smu_read_adc(sc, edata->private);
		if (r != -1) {
			r = r * sc->cpu_m;
			r >>= 3;
			r += (int64_t)sc->cpu_b << 9;
			r <<= 1;
			r *= 15625;
			r /= 1024;
			edata->value_cur = r + 273150000;
			edata->state = ENVSYS_SVALID;
		}
	}
}

static int
smu_do_cmd(struct smu_softc *sc, struct smu_cmd *cmd, int timo)
{
	int gpio, ret, bail;
	u_char ack;

	mutex_enter(&sc->sc_cmd_lock);

	DPRINTF("%s: cmd %02x len %02x\n", __func__, cmd->cmd, cmd->len);
	DPRINTF("%s: data %02x %02x %02x %02x %02x %02x %02x %02x\n", __func__,
	    cmd->data[0], cmd->data[1], cmd->data[2], cmd->data[3],
	    cmd->data[4], cmd->data[5], cmd->data[6], cmd->data[7]);

	sc->sc_cmd->cmd = cmd->cmd;
	sc->sc_cmd->len = cmd->len;
	memcpy(sc->sc_cmd->data, cmd->data, cmd->len);

	__asm volatile ("dcbf 0,%0; sync" :: "r"(sc->sc_cmd) : "memory");

	obio_write_4(sc->sc_dbell_mbox, sc->sc_cmd_paddr);
	obio_write_1(sc->sc_dbell_gpio, 0x04);

	bail = 0;

	gpio = obio_read_1(sc->sc_dbell_gpio);

	while (((gpio & 0x07) != 0x07) && (bail < timo)) {
		ret = tsleep(sc->sc_cmd, PWAIT, "smu_cmd", mstohz(10));
		if (ret != 0) {
			bail++;
		}
		gpio = obio_read_1(sc->sc_dbell_gpio);
	}

	if ((gpio & 0x07) != 0x07) {
		mutex_exit(&sc->sc_cmd_lock);
		return EWOULDBLOCK;
	}
		
	__asm volatile ("dcbf 0,%0; sync" :: "r"(sc->sc_cmd) : "memory");

	ack = (~cmd->cmd) & 0xff;
	if (sc->sc_cmd->cmd != ack) {
		DPRINTF("%s: invalid ack, got %x expected %x\n",
		    __func__, sc->sc_cmd->cmd, ack);
		mutex_exit(&sc->sc_cmd_lock);
		return EIO;
	}

	cmd->cmd = sc->sc_cmd->cmd;
	cmd->len = sc->sc_cmd->len;
	memcpy(cmd->data, sc->sc_cmd->data, sc->sc_cmd->len);

	mutex_exit(&sc->sc_cmd_lock);

	return 0;
}


static int
smu_dbell_gpio_intr(void *arg)
{
	struct smu_softc *sc = arg;

	DPRINTF("%s\n", __func__);

	wakeup(sc->sc_cmd);

	return 1;
}

void
smu_poweroff(void)
{
	struct smu_cmd cmd;

	if (smu0 == NULL)
		return;

	cmd.cmd = SMU_CMD_POWER;
	strcpy(cmd.data, "SHUTDOWN");
	cmd.len = strlen(cmd.data) + 1;
	smu_do_cmd(smu0, &cmd, 800);

	for (;;);
}

void
smu_restart(void)
{
	struct smu_cmd cmd;

	if (smu0 == NULL)
		return;

	cmd.cmd = SMU_CMD_POWER;
	strcpy(cmd.data, "RESTART");
	cmd.len = strlen(cmd.data) + 1;
	smu_do_cmd(smu0, &cmd, 800);

	for (;;);
}

static int
smu_todr_gettime_ymdhms(todr_chip_handle_t tch, struct clock_ymdhms *dt)
{
	struct smu_softc *sc = tch->cookie;
	struct smu_cmd cmd;
	int ret;

	cmd.cmd = SMU_CMD_RTC;
	cmd.len = 1;
	cmd.data[0] = 0x81;

	ret = smu_do_cmd(sc, &cmd, 800);
	if (ret != 0)
		return ret;

	dt->dt_sec = bcdtobin(cmd.data[0]);
	dt->dt_min = bcdtobin(cmd.data[1]);
	dt->dt_hour = bcdtobin(cmd.data[2]);
	dt->dt_wday = bcdtobin(cmd.data[3]);
	dt->dt_day = bcdtobin(cmd.data[4]);
	dt->dt_mon = bcdtobin(cmd.data[5]);
	dt->dt_year = bcdtobin(cmd.data[6]) + 2000;

	return 0;
}

static int
smu_todr_settime_ymdhms(todr_chip_handle_t tch, struct clock_ymdhms *dt)
{
	struct smu_softc *sc = tch->cookie;
	struct smu_cmd cmd;

	cmd.cmd = SMU_CMD_RTC;
	cmd.len = 8;
	cmd.data[0] = 0x80;
	cmd.data[1] = bintobcd(dt->dt_sec);
	cmd.data[2] = bintobcd(dt->dt_min);
	cmd.data[3] = bintobcd(dt->dt_hour);
	cmd.data[4] = bintobcd(dt->dt_wday);
	cmd.data[5] = bintobcd(dt->dt_day);
	cmd.data[6] = bintobcd(dt->dt_mon);
	cmd.data[7] = bintobcd(dt->dt_year - 2000);

	return smu_do_cmd(sc, &cmd, 800);
}

static int
smu_fan_update_rpm(struct smu_fan *fan)
{
	struct smu_softc *sc = fan->sc;
	struct smu_cmd cmd;
	int ret, diff;

	cmd.cmd = SMU_CMD_FAN;
	cmd.len = 2;
	cmd.data[0] = 0x31;
	cmd.data[1] = fan->reg;

	ret = smu_do_cmd(sc, &cmd, 800);
	if (ret == 0) {
		fan->last_update = time_uptime;
		fan->current_rpm = (cmd.data[0] << 8) | cmd.data[1];
	} else {
		cmd.cmd = SMU_CMD_FAN;
		cmd.len = 1;
		cmd.data[0] = 0x01;

		ret = smu_do_cmd(sc, &cmd, 800);
		if (ret == 0) {
			fan->last_update = time_uptime;
			fan->current_rpm = (cmd.data[1 + fan->reg * 2] << 8) |
			    cmd.data[2 + fan->reg * 2];
		}
	}
	diff = abs(fan->current_rpm - fan->wanted_rpm);
	if (diff > fan->max_rpm >> 3) {
		fan->fault++;
	} else fan->fault = 0;
	return ret;
}

static int
smu_fan_get_rpm(void *cookie, int which)
{
	struct smu_softc *sc = cookie;
	struct smu_fan *fan = &sc->sc_fans[which];
	int ret;
	ret = 0;

	if (time_uptime - fan->last_update > 1) {
		ret = smu_fan_update_rpm(fan);
		if (ret != 0)
			return -1;
	}

	return fan->current_rpm;
}

static int
smu_fan_set_rpm(void *cookie, int which, int rpm)
{
	struct smu_softc *sc = cookie;
	struct smu_fan *fan = &sc->sc_fans[which];
	struct smu_cmd cmd;
	int ret;

	DPRINTF("%s: fan %s rpm %d\n", __func__, fan->location, rpm);

	rpm = uimax(fan->min_rpm, rpm);
	rpm = uimin(fan->max_rpm, rpm);

	fan->wanted_rpm = rpm;

	cmd.cmd = SMU_CMD_FAN;
	cmd.len = 4;
	cmd.data[0] = 0x30;
	cmd.data[1] = fan->reg;
	cmd.data[2] = (rpm >> 8) & 0xff;
	cmd.data[3] = rpm & 0xff;

	ret = smu_do_cmd(sc, &cmd, 800);
	if (ret != 0) {
		cmd.cmd = SMU_CMD_FAN;
		cmd.len = 14;
		cmd.data[0] = fan->rpm_ctl ? 0x00 : 0x10;
		cmd.data[1] = 1 << fan->reg;
		cmd.data[2] = cmd.data[2 + fan->reg * 2] = (rpm >> 8) & 0xff;
		cmd.data[3] = cmd.data[3 + fan->reg * 2] = rpm & 0xff;

		ret = smu_do_cmd(sc, &cmd, 800);
	}

	return ret;
}

static int
smu_read_adc(struct smu_softc *sc, int id)
{
	struct smu_cmd cmd;
	int ret;

	cmd.cmd = SMU_CMD_ADC;
	cmd.len = 1;
	cmd.data[0] = id;

	ret = smu_do_cmd(sc, &cmd, 800);
	if (ret == 0) {
		return cmd.data[0] << 8 | cmd.data[1];
	}
	return -1;
}

static int
smu_iicbus_exec(void *cookie, i2c_op_t op, i2c_addr_t addr, const void *send,
    size_t send_len, void *recv, size_t recv_len, int flags)
{
	struct smu_iicbus *iicbus = cookie;
	struct smu_softc *sc = iicbus->sc;
	struct smu_cmd cmd;
	int retries, ret;

	DPRINTF("%s: op %x addr %x send_len %d recv_len %d\n",
	    __func__, op, addr, send_len, recv_len);

	cmd.cmd = SMU_CMD_I2C;
	cmd.len = 9 + recv_len;
	cmd.data[0] = iicbus->reg;
	cmd.data[1] = I2C_OP_READ_P(op) ? 0x02 : 0x00;
	cmd.data[2] = addr << 1;
	cmd.data[3] = send_len;
	memcpy(&cmd.data[4], send, send_len);
	cmd.data[7] = addr << 1;
	if (I2C_OP_READ_P(op))
		cmd.data[7] |= 0x01;
	cmd.data[8] = recv_len;
	memcpy(&cmd.data[9], recv, recv_len);

	ret = smu_do_cmd(sc, &cmd, 800);
	if (ret != 0)
		return (ret);

	for (retries = 0; retries < 10; retries++) {
		cmd.cmd = SMU_CMD_I2C;
		cmd.len = 1;
		cmd.data[0] = 0x00;
		memset(&cmd.data[1], 0xff, recv_len);

		ret = smu_do_cmd(sc, &cmd, 800);

		DPRINTF("%s: cmd data[0] %x\n", __func__, cmd.data[0]);

		if (ret == 0 && (cmd.data[0] & 0x80) == 0)
			break;

		DELAY(10000);
	}

	if (cmd.data[0] & 0x80)
		return EIO;

	if (I2C_OP_READ_P(op))
		memcpy(recv, &cmd.data[1], recv_len);

	return 0;
}

SYSCTL_SETUP(smu_sysctl_setup, "SMU sysctl subtree setup")
{
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "machdep", NULL,
	    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_EOL);
}

static void
smu_setup_zones(struct smu_softc *sc)
{
	struct smu_fan *f;
	fancontrol_zone_t *z;
	int i;

	/* init zones */
	sc->sc_zones[SMU_ZONE_CPU].name = "CPUs";
	sc->sc_zones[SMU_ZONE_CPU].filter = is_cpu_sensor;
	sc->sc_zones[SMU_ZONE_CPU].cookie = sc;
	sc->sc_zones[SMU_ZONE_CPU].get_rpm = smu_fan_get_rpm;
	sc->sc_zones[SMU_ZONE_CPU].set_rpm = smu_fan_set_rpm;
	sc->sc_zones[SMU_ZONE_CPU].Tmin = 45;
	sc->sc_zones[SMU_ZONE_CPU].Tmax = 80;
	sc->sc_zones[SMU_ZONE_CPU].nfans = 0;
	sc->sc_zones[SMU_ZONE_CASE].name = "Slots";
	sc->sc_zones[SMU_ZONE_CASE].filter = is_slots_sensor;
	sc->sc_zones[SMU_ZONE_CASE].cookie = sc;
	sc->sc_zones[SMU_ZONE_CASE].Tmin = 50;
	sc->sc_zones[SMU_ZONE_CASE].Tmax = 75;
	sc->sc_zones[SMU_ZONE_CASE].nfans = 0;
	sc->sc_zones[SMU_ZONE_CASE].get_rpm = smu_fan_get_rpm;
	sc->sc_zones[SMU_ZONE_CASE].set_rpm = smu_fan_set_rpm;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].name = "Drivebays";
	sc->sc_zones[SMU_ZONE_DRIVEBAY].filter = is_drive_sensor;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].cookie = sc;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].get_rpm = smu_fan_get_rpm;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].set_rpm = smu_fan_set_rpm;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].Tmin = 30;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].Tmax = 50;
	sc->sc_zones[SMU_ZONE_DRIVEBAY].nfans = 0;

	/* find CPU fans */
	z = &sc->sc_zones[SMU_ZONE_CPU];
	for (i = 0; i < SMU_MAX_FANS; i++) {
		f = &sc->sc_fans[i];
		if ((strstr(f->location, "CPU") != NULL) || 
		    (strstr(f->location, "System") != NULL)) {
			z->fans[z->nfans].num = i;
			z->fans[z->nfans].min_rpm = f->min_rpm;
			z->fans[z->nfans].max_rpm = f->max_rpm;
			z->fans[z->nfans].name = f->location;
			z->nfans++;
		}
	}
	aprint_normal_dev(sc->sc_dev,
	    "using %d fans for CPU zone\n", z->nfans);

	z = &sc->sc_zones[SMU_ZONE_DRIVEBAY];
	for (i = 0; i < SMU_MAX_FANS; i++) {
		f = &sc->sc_fans[i];
		if ((strstr(f->location, "DRIVE") != NULL) ||
		    (strstr(f->location, "Drive") != NULL)) {
			z->fans[z->nfans].num = i;
			z->fans[z->nfans].min_rpm = f->min_rpm;
			z->fans[z->nfans].max_rpm = f->max_rpm;
			z->fans[z->nfans].name = f->location;
			z->nfans++;
		}
	}
	aprint_normal_dev(sc->sc_dev,
	    "using %d fans for drive bay zone\n", z->nfans);

	z = &sc->sc_zones[SMU_ZONE_CASE];
	for (i = 0; i < SMU_MAX_FANS; i++) {
		f = &sc->sc_fans[i];
		if ((strstr(f->location, "BACKSIDE") != NULL) ||
		    (strstr(f->location, "SLOTS") != NULL)) {
			z->fans[z->nfans].num = i;
			z->fans[z->nfans].min_rpm = f->min_rpm;
			z->fans[z->nfans].max_rpm = f->max_rpm;
			z->fans[z->nfans].name = f->location;
			z->nfans++;
		}
	}
	aprint_normal_dev(sc->sc_dev,
	    "using %d fans for expansion slots zone\n", z->nfans);

	/* setup sysctls for our zones etc. */
	for (i = 0; i < SMU_ZONES; i++) {
		fancontrol_init_zone(&sc->sc_zones[i], sc->sc_sysctl_me);
	}

	sc->sc_dying = false;
	kthread_create(PRI_NONE, 0, curcpu(), smu_adjust, sc, &sc->sc_thread,
	    "fan control"); 
}

static void
smu_adjust(void *cookie)
{
	struct smu_softc *sc = cookie;
	int i;

	while (!sc->sc_dying) {
		for (i = 0; i < SMU_ZONES; i++)
			if (sc->sc_zones[i].nfans > 0)
				fancontrol_adjust_zone(&sc->sc_zones[i]);
		kpause("fanctrl", true, mstohz(2000), NULL);
	}
	kthread_exit(0);
}

static bool is_cpu_sensor(const envsys_data_t *edata)
{
	if (edata->units != ENVSYS_STEMP)
		return false;
	if (strstr(edata->desc, "CPU") != NULL)
		return TRUE;
	return false;
}

static bool is_drive_sensor(const envsys_data_t *edata)
{
	if (edata->units != ENVSYS_STEMP)
		return false;
	if (strstr(edata->desc, "DRIVE") != NULL)
		return TRUE;
	if (strstr(edata->desc, "drive") != NULL)
		return TRUE;
	return false;
}

static bool is_slots_sensor(const envsys_data_t *edata)
{
	if (edata->units != ENVSYS_STEMP)
		return false;
	if (strstr(edata->desc, "BACKSIDE") != NULL)
		return TRUE;
	if (strstr(edata->desc, "INLET") != NULL)
		return TRUE;
	if (strstr(edata->desc, "DIODE") != NULL)
		return TRUE;
	if (strstr(edata->desc, "TUNNEL") != NULL)
		return TRUE;
	return false;
}

int
smu_get_datablock(int id, uint8_t *buf, size_t len)
{
	struct smu_cmd cmd;

	cmd.cmd = SMU_PARTITION;
	cmd.len = 2;
	cmd.data[0] = SMU_PARTITION_LATEST;
	cmd.data[1] = id;
	smu_do_cmd(smu0, &cmd, 100);

	cmd.data[4] = cmd.data[0];
	cmd.data[5] = cmd.data[1];

	cmd.cmd = SMU_MISC;
	cmd.len = 7;
	cmd.data[0] = SMU_MISC_GET_DATA;
	cmd.data[1] = 4;
	cmd.data[2] = 0;
	cmd.data[3] = 0;
	cmd.data[6] = len;
	smu_do_cmd(smu0, &cmd, 100);

	memcpy(buf, cmd.data, len);
	return 0;
}
