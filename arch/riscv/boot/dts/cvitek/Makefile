# SPDX-License-Identifier: GPL-2.0
dtb-y += sg2002_licheervnano_sd.dtb

nanokvm-ion-layout := $(obj)/onekvm_nanokvm_ion_layout.dtsi

targets += onekvm_nanokvm_ion_layout.dtsi

quiet_cmd_nanokvm_ion_layout = GEN     $@
      cmd_nanokvm_ion_layout = $(PYTHON3) $(src)/nanokvm_ion_layout.py \
		--ion-size-mib $(CONFIG_ION_SIZE_MIB) \
		--rtos-ram-size-mib $(CONFIG_RTOS_RAM_SIZE_MIB) \
		--framebuffer-size-mib $(if $(CONFIG_FRAMEBUFFER_RESERVE),$(CONFIG_FRAMEBUFFER_SIZE_MIB),0) \
		--gpio-i2c-delay-us $(CONFIG_GPIO_I2C_DELAY_US) \
		--output $@

$(nanokvm-ion-layout): $(src)/nanokvm_ion_layout.py FORCE
	$(call if_changed,nanokvm_ion_layout)

$(obj)/sg2002_licheervnano_sd.dtb: $(nanokvm-ion-layout)

always := $(dtb-y)
clean-files := *.dtb onekvm_nanokvm_ion_layout.dtsi
