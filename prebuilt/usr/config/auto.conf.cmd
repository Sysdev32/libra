autoconfig := include/config/auto.conf

deps_config := \
	Kconfig \

$(autoconfig): $(deps_config)
$(deps_config): ;
