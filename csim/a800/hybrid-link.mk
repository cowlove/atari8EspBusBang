include Makefile

.PHONY: atari800-hybrid

atari800-hybrid: $(atari800_OBJECTS) $(atari800_DEPENDENCIES) $(CSIM_OBJECTS)
	$(CXX) $(LDFLAGS) -o $(HYBRID_BIN) $(atari800_OBJECTS) $(atari800_LDADD) $(LIBS) $(CSIM_OBJECTS) -lz
