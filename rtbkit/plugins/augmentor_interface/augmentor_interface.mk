$(eval $(call library,http_augmentor,http_augmentor_interface.cc,rtb_router openrtb_bid_request))

augmentor_interface_plugins: $(LIB)/libhttp_augmentor.so

.PHONY: augmentor_interface_plugins
