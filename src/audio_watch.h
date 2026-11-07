#pragma once
// Sound devices (Windows): the game's sound is DirectSound, opened once on the
// default output device, and nothing in the game follows a new default
// (headphones plugged in after the game started: no sound until a restart,
// Facepunch issue #6388, where snd_restart is the fix). Windows' endpoint
// notifications (IMMNotificationClient) are watched on a thread of their own;
// the menu reads how many times the default output changed and runs
// snd_restart (doctor.lua). The callback only counts: it never blocks or
// calls the game.
//
// And for the PC doctor's sound section: the default output and microphone,
// Windows' communications microphone, and whether either is a Bluetooth
// headset's hands-free endpoint (phone-quality sound while the mic is open).
// Voice chat records through Steam (ISteamUser::GetVoice), not the game.
// Names stay on this PC.
#include <string>

namespace audio_watch
{
	// Starts watching (once; later calls do nothing). False if Windows' audio
	// endpoints can't be watched.
	bool Start();

	// How many times Windows' default output (console role) changed since Start.
	unsigned Changes();

	struct Devices
	{
		bool ok = false;                 // the endpoints could be read
		std::string output;              // UTF-8 friendly names; "" = none
		std::string input;
		std::string commsInput;          // Windows' default communications microphone
		bool outputHandsFree = false;
		bool inputHandsFree = false;
		int inputs = 0;                  // active capture endpoints
	};

	// Reads them now (on a short-lived thread of its own, for COM).
	Devices Read();

	std::string Describe();
}
