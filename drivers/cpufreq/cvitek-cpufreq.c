// SPDX-License-Identifier: GPL-2.0-only
/*
 * Report-only cpufreq for Cvitek/Sophgo CV181x and SG200x.
 *
 * Expose the C906 clock through cpufreq sysfs so lscpu can print
 * CPU max/min MHz. Do not change the CPU rate: mux/PLL updates
 * require an XTAL bypass that generic clk_set_rate() does not do.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/module.h>

#define CVITEK_CPU_CLK_NAME "clk_c906_0"

static struct clk *cpu_clk;
static struct cpufreq_frequency_table freq_table[2];

static unsigned int cvitek_cpufreq_get(unsigned int cpu)
{
	unsigned long rate;

	if (!cpu_clk)
		return 0;

	rate = clk_get_rate(cpu_clk);
	return (unsigned int)(rate / 1000);
}

static int cvitek_cpufreq_target(struct cpufreq_policy *policy,
				 unsigned int index)
{
	return 0;
}

static int cvitek_cpufreq_init(struct cpufreq_policy *policy)
{
	if (!freq_table[0].frequency)
		return -ENODEV;

	policy->clk = cpu_clk;
	cpufreq_generic_init(policy, freq_table, CPUFREQ_ETERNAL);
	return 0;
}

static struct cpufreq_driver cvitek_cpufreq_driver = {
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_CONST_LOOPS,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= cvitek_cpufreq_target,
	.get		= cvitek_cpufreq_get,
	.init		= cvitek_cpufreq_init,
	.name		= "cvitek-cpufreq",
};

static int __init cvitek_cpufreq_register(void)
{
	unsigned long rate;
	int ret;

	cpu_clk = clk_get(NULL, CVITEK_CPU_CLK_NAME);
	if (IS_ERR(cpu_clk)) {
		pr_info("clk %s not available (%ld)\n",
			CVITEK_CPU_CLK_NAME, PTR_ERR(cpu_clk));
		cpu_clk = NULL;
		return 0;
	}

	rate = clk_get_rate(cpu_clk);
	if (!rate) {
		clk_put(cpu_clk);
		cpu_clk = NULL;
		return 0;
	}

	freq_table[0].frequency = rate / 1000;
	freq_table[1].frequency = CPUFREQ_TABLE_END;

	ret = cpufreq_register_driver(&cvitek_cpufreq_driver);
	if (ret) {
		clk_put(cpu_clk);
		cpu_clk = NULL;
		pr_err("failed to register cpufreq driver: %d\n", ret);
		return ret;
	}

	pr_info("reporting %lu kHz (read-only)\n", rate / 1000);
	return 0;
}
device_initcall(cvitek_cpufreq_register);
