SHELL := /bin/bash
.DEFAULT_GOAL := all

LWIP_SRC := lwip/src
LWIP_OBJ := lwip/build
LWIP_LIB := $(LWIP_OBJ)/liblwip.a
LWIP_FLAGS := $(OPTIMIZE) -ggdb -fno-omit-frame-pointer -mgeneral-regs-only \
	-std=gnu11 -Wall -Werror -D__KERNEL__ -Isrc/include -Isrc/net/include \
	-I$(LWIP_SRC)/include

CORE := def inet_chksum init ip mem memp netif pbuf stats sys tcp tcp_in tcp_out timeouts udp
IPV4 := acd dhcp etharp icmp ip4 ip4_addr
OBJS := $(addprefix $(LWIP_OBJ)/core/,$(addsuffix .o,$(CORE))) \
	$(addprefix $(LWIP_OBJ)/core/ipv4/,$(addsuffix .o,$(IPV4))) \
	$(LWIP_OBJ)/netif/ethernet.o

.PHONY: all clean FORCE
all: $(LWIP_LIB)

$(LWIP_OBJ)/flags: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(LWIP_FLAGS) STACK_PROTECTOR=$(STACK_PROTECTOR)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

$(LWIP_LIB): $(OBJS) lwip/lwip.mk
	$(RM) $@
	$(AR) rcs $@ $(OBJS)

$(LWIP_OBJ)/%.o: $(LWIP_SRC)/%.c $(LWIP_OBJ)/flags
	@mkdir -p $(@D)
	@$(CC) $(LWIP_FLAGS) -MMD -MP -c -o $@ $<
	@echo $@

clean:
	$(RM) -r $(LWIP_OBJ)

-include $(OBJS:.o=.d)
