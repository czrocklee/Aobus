@0x8e501efc7848a202;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("rml");

struct AudioSource
{
  filePath @0 :Text;
  lastModified @1 :UInt64;
  length @2 :UInt32;
  bitRate @3 :UInt32;
  sampleRate @4 :UInt32;
  bitDepth @5 :UInt8;
}

struct Classical
{
  composer @0 :Text;
  conductor @1 :Text;
  orchestra @2 :Text;
}

struct Track
{
  title @0 :Text;
  artist @1 :Text;
  artists @2 :List(Text);
  album @3 :Text;
  albumArtist @4 :Text;
  genre @5 :Text;

  trackNumber @6 :UInt16;
  totalTracks @7 :UInt16;
  diskNumber @8 :UInt16;
  totalDisks @9 :UInt16;

  source @10 :AudioSource;
  sources @11 :List(AudioSource);
}
