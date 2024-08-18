# Compiler
CC = gcc
# Compiler flags
CFLAGS = -Wall -Wextra -pedantic -std=c99 -O2
# Linker flags
LDFLAGS = -lm -pthread
# SDL2 flags
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LDFLAGS = $(shell sdl2-config --libs)
# SDL2_ttf flags
TTF_CFLAGS = $(shell pkg-config --cflags SDL2_ttf)
TTF_LDFLAGS = $(shell pkg-config --libs SDL2_ttf)
# Source files
SERVER_SRC = server.c
CLIENT_SRC = client.c
# Executables
SERVER = server
CLIENT = client
APPLICATION = Wormio.app/Contents/MacOS/Wormio
# DMG settings
DMG_NAME = Wormio
VOLUME_NAME = "Wormio"
DMG_DIR = Wormio_DMG
# Code signing identity
CODESIGN_IDENTITY = "Apple Development: jacobbcahoon+apple@gmail.com"

# Default target
all: $(SERVER) $(APPLICATION) $(CLIENT) copy_frameworks update_rpath create_info_plist package_font codesign create_dmg

# Server compilation
$(SERVER): $(SERVER_SRC)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS)

# Client compilation
$(CLIENT): $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS) $(TTF_LDFLAGS)

# Client compilation (directly into app bundle)
$(APPLICATION): $(CLIENT_SRC)
	mkdir -p Wormio.app/Contents/MacOS
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS) $(TTF_LDFLAGS)

# Copy SDL2 and SDL2_ttf frameworks into app bundle
copy_frameworks:
	mkdir -p Wormio.app/Contents/Frameworks
	cp -R $(shell sdl2-config --prefix)/lib/libSDL2-2.0.0.dylib Wormio.app/Contents/Frameworks/
	cp -R $(shell pkg-config --variable=libdir SDL2_ttf)/libSDL2_ttf-2.0.0.dylib Wormio.app/Contents/Frameworks/

# Update executable's library paths
update_rpath:
	install_name_tool -change $(shell sdl2-config --prefix)/lib/libSDL2-2.0.0.dylib @executable_path/../Frameworks/libSDL2-2.0.0.dylib $(APPLICATION)
	install_name_tool -change $(shell pkg-config --variable=libdir SDL2_ttf)/libSDL2_ttf-2.0.0.dylib @executable_path/../Frameworks/libSDL2_ttf-2.0.0.dylib $(APPLICATION)

# Create Info.plist
create_info_plist:
	echo '<?xml version="1.0" encoding="UTF-8"?>\
	<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\
	<plist version="1.0">\
	<dict>\
		<key>CFBundleDevelopmentRegion</key>\
		<string>en</string>\
		<key>CFBundleExecutable</key>\
		<string>Wormio</string>\
		<key>CFBundleIconFile</key>\
		<string>Wormio.icns</string>\
		<key>CFBundleIdentifier</key>\
		<string>com.yourdomain.wormio</string>\
		<key>CFBundleInfoDictionaryVersion</key>\
		<string>6.0</string>\
		<key>CFBundleName</key>\
		<string>Wormio</string>\
		<key>CFBundlePackageType</key>\
		<string>APPL</string>\
		<key>CFBundleShortVersionString</key>\
		<string>1.0</string>\
		<key>CFBundleVersion</key>\
		<string>1</string>\
		<key>LSMinimumSystemVersion</key>\
		<string>10.9</string>\
		<key>NSHighResolutionCapable</key>\
		<true/>\
	</dict>\
	</plist>' > Wormio.app/Contents/Info.plist

# Package font
package_font:
	mkdir -p Wormio.app/Contents/Resources
	cp cmunui.ttf Wormio.app/Contents/Resources/

# Code sign the app
codesign:
	codesign --force --sign $(CODESIGN_IDENTITY) Wormio.app

# Create DMG
create_dmg:
	@echo "Creating DMG..."
	rm -rf $(DMG_DIR)
	mkdir -p $(DMG_DIR)
	cp -R Wormio.app $(DMG_DIR)/
	ln -s /Applications $(DMG_DIR)/Applications
	hdiutil create -volname $(VOLUME_NAME) -srcfolder $(DMG_DIR) -ov -format UDZO $(DMG_NAME).dmg
	rm -rf $(DMG_DIR)
	@echo "DMG created: $(DMG_NAME).dmg"

# Clean up
clean:
	rm -f $(SERVER)
	rm -rf Wormio.app
	rm -f $(DMG_NAME).dmg
	rm -f $(CLIENT)

# Phony targets
.PHONY: all clean copy_frameworks update_rpath create_info_plist package_font codesign create_dmg
