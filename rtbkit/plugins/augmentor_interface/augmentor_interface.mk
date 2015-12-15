$(eval $(call library,http_augmentor,http_augmentor_interface.cc,rtb_router openrtb_bid_request))
$(eval $(call library,zmq_augmentor,zmq_augmentor_interface.cc,rtb_router openrtb_bid_request))

augmentor_interface_plugins: $(LIB)/libhttp_augmentor.so $(LIB)/libzmq_augmentor.so

.PHONY: augmentor_interface_plugins
