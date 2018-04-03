@0x8e501efc7848a202;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("rs::ml::core");

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
  album @2 :Text;
  albumArtist @3 :Text;
  genre @4 :Text;

  trackNumber @5 :UInt16;
  totalTracks @6 :UInt16;
  diskNumber @7 :UInt16;
  totalDisks @8 :UInt16;

  classical @9 :Classical;
  source @10 :AudioSource;
}
