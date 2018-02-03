This is a fork of Rhythmbox (https://github.com/GNOME/rhythmbox) 
for the development of a Deezer plugin.

It uses the Deezer API & native SDK (https://developers.deezer.com/sdk/native)
The SDK includes a proprietary binary, as such this plugin cannot be a part 
of the official Rhythmbox source.

If you're building on Debian x86_64 like me, you can run the following commands
to build and install rhythmbox with Deezer support:

Install dependencies
```
sudo apt build-dep rhythmbox
```

Install libssl1.0.0 (required for Deezer SDK, 
see https://github.com/deezer/native-sdk-samples/issues/23)
```
sudo apt install libssl1.0.0
```
NB if this package isn't available, you'll have to find it elsewhere. 
I had to take it from an older version of debian.
https://packages.debian.org/jessie/amd64/libssl1.0.0/download

Download the source code
```
git clone https://github.com/MFAshby/rhythmbox-deezer.git
```

Extract the deezer libraries, header files and autotools 
configuration to your system
NB this zip only contains the x86_64 linux binary. 
See the Deezer SDK documentation for other systems
```
cd rhythmbox-deezer/
sudo tar xf plugins/deezerpl/libdeezer.tgz -C / 
```

Build & install - change the j parameter for your number of CPU cores
```
autoreconf -i
./configure
make -j8
sudo make install
```
