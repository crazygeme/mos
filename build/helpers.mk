SUBDIR_CFLAGS_FILES = $(shell find src/ -name 'cflags.mk' | sort)

dir_cflags_var = CFLAGS_$(subst /,_,$(patsubst %/,%,$(1)))
dir_parent = $(patsubst %/,%,$(dir $(patsubst %/,%,$(1))))
dir_cflags_walk = \
	$(if $(filter .,$(1)),,$(call dir_cflags_walk,$(call dir_parent,$(1))) \
	$($(call dir_cflags_var,$(1))))
dir_cflag_file = $(patsubst %/,%,$(1))/cflags.mk
dir_cflag_files_walk = \
	$(if $(filter .,$(1)),,$(call dir_cflag_files_walk,$(call dir_parent,$(1))) \
	$(wildcard $(call dir_cflag_file,$(1))))
dir_cflags = $(strip $(CFLAGS) \
	$(call dir_cflags_walk,$(patsubst %/,%,$(dir $(1)))))

define load_subdir_cflags
__saved_cflags := $(CFLAGS)
CFLAGS :=
include $(1)
$(call dir_cflags_var,$(patsubst %/cflags.mk,%,$(1))) := $$(strip $$(CFLAGS))
CFLAGS := $$(__saved_cflags)
endef

$(foreach file,$(SUBDIR_CFLAGS_FILES),$(eval $(call load_subdir_cflags,$(file))))
