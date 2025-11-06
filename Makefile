PLUGIN = mydrumkit
CXXFLAGS += -fPIC -O2 -I/usr/include/lv2
LDFLAGS += -shared -lsndfile

$(PLUGIN).so: $(PLUGIN).cpp
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LDFLAGS)

install:
	mkdir -p ~/.lv2/$(PLUGIN).lv2
	cp $(PLUGIN).so manifest.ttl $(PLUGIN).ttl ~/.lv2/$(PLUGIN).lv2/
	cp -r samples ~/.lv2/$(PLUGIN).lv2/

clean:
	rm -f $(PLUGIN).so

uninstall:
	rm -r ~/.lv2/$(PLUGIN).lv2/
