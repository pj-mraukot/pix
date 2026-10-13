# C++14: in C++17 std::byte clashes with our 'byte' typedef because of 'using namespace std'
CXX      = g++
CXXFLAGS = -std=c++14 -O2 -Wall -Wextra
LDLIBS   = -lpng -ljpeg -lz

# object files and their dependency files go into build/, the program into the top directory
BUILD = build
SRCS  = src/main.cpp src/image_io.cpp src/pix_codec.cpp src/pix2_codec.cpp src/pix_error.cpp src/pix_report.cpp
OBJS  = $(SRCS:src/%.cpp=$(BUILD)/%.o)

pix: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)

# -MMD -MP: the compiler lists the headers each object depends on (build/*.d)
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $@

-include $(OBJS:.o=.d)

# Qt 6 viewer, built with CMake into viewer/build
viewer:
	cmake -S viewer -B viewer/build -DCMAKE_BUILD_TYPE=Release
	cmake --build viewer/build -j

# Windows viewer (Win32 API, no Qt), cross-compiled with MinGW-w64 into winview/build/pixview.exe:
# one static .exe (needs: sudo apt install g++-mingw-w64-x86-64 libz-mingw-w64-dev)
WINCXX    = x86_64-w64-mingw32-g++
WINDRES   = x86_64-w64-mingw32-windres
WINBUILD  = winview/build
WINFLAGS  = -std=c++14 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -municode
WINLIBS   = -s -static -mwindows -lz -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lcomctl32 -lgdi32
WINSRCS   = winview/main.cpp winview/main_window.cpp winview/image_view.cpp winview/file_dialog.cpp winview/picture.cpp
CODECSRCS = src/pix_api.cpp src/pix_codec.cpp src/pix2_codec.cpp src/pix_error.cpp src/pix_report.cpp
WINOBJS   = $(WINSRCS:winview/%.cpp=$(WINBUILD)/%.o) $(CODECSRCS:src/%.cpp=$(WINBUILD)/codec/%.o)

winview: $(WINBUILD)/pixview.exe

$(WINBUILD)/pixview.exe: $(WINOBJS) $(WINBUILD)/resources.o
	$(WINCXX) $(WINFLAGS) -o $@ $(WINOBJS) $(WINBUILD)/resources.o $(WINLIBS)

$(WINBUILD)/%.o: winview/%.cpp | $(WINBUILD)
	$(WINCXX) $(WINFLAGS) -MMD -MP -c -o $@ $<

$(WINBUILD)/codec/%.o: src/%.cpp | $(WINBUILD)/codec
	$(WINCXX) $(WINFLAGS) -MMD -MP -c -o $@ $<

$(WINBUILD)/resources.o: winview/pixview.rc winview/pixview.manifest winview/pixview.ico | $(WINBUILD)
	$(WINDRES) --include-dir winview -O coff -o $@ $<

$(WINBUILD) $(WINBUILD)/codec:
	mkdir -p $@

-include $(WINOBJS:.o=.d)

clean:
	rm -f pix
	rm -rf $(BUILD) viewer/build $(WINBUILD)

.PHONY: clean viewer winview
