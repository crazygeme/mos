SUBDIR_CFLAGS_FILES = $(shell find src/ -name 'cflags.mk' | sort)

dir_cflags_var = CFLAGS_$(subst /,_,$(patsubst %/,%,$(1)))
dir_build_cflags_var = $(call dir_cflags_var,$(1))_$(2)
dir_parent = $(patsubst %/,%,$(dir $(patsubst %/,%,$(1))))
dir_cflags_walk = \
	$(if $(filter .,$(1)),,$(call dir_cflags_walk,$(call dir_parent,$(1))) \
	$($(call dir_cflags_var,$(1))) \
	$($(call dir_build_cflags_var,$(1),$(BUILD))))
dir_cflag_file = $(patsubst %/,%,$(1))/cflags.mk
dir_cflag_files_walk = \
	$(if $(filter .,$(1)),,$(call dir_cflag_files_walk,$(call dir_parent,$(1))) \
	$(wildcard $(call dir_cflag_file,$(1))))
dir_cflags = $(strip $(COMMON_CFLAGS) \
	$(call dir_cflags_walk,$(patsubst %/,%,$(dir $(1)))))

define load_subdir_cflags
__saved_cflags := $(CFLAGS)
CFLAGS :=
CFLAGS-debug :=
CFLAGS-release :=
include $(1)
$(call dir_cflags_var,$(patsubst %/cflags.mk,%,$(1))) := $$(strip $$(CFLAGS))
$(call dir_build_cflags_var,$(patsubst %/cflags.mk,%,$(1)),debug) := $$(strip $$(CFLAGS-debug))
$(call dir_build_cflags_var,$(patsubst %/cflags.mk,%,$(1)),release) := $$(strip $$(CFLAGS-release))
CFLAGS := $$(__saved_cflags)
endef

$(foreach file,$(SUBDIR_CFLAGS_FILES),$(eval $(call load_subdir_cflags,$(file))))
