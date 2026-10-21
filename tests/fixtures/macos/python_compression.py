"""Exercise the codecs already compiled into the bundled Mach-O extensions."""
import bz2
import gzip
import lzma
import zlib

payload = (b"Alchemy\x00ARM64 guest compression\n" * 128) + bytes(range(256)) * 4

for level in (0, 6, 9):
    compressed = zlib.compress(payload, level)
    assert zlib.decompress(compressed) == payload

assert zlib.crc32(b"123456789") == 0xCBF43926
assert zlib.adler32(b"123456789") == 0x091E01DE
dictionary = b"Alchemy ARM64 guest compression"
encoder = zlib.compressobj(zdict=dictionary)
encoded = encoder.compress(payload[:127]) + encoder.compress(payload[127:]) + encoder.flush()
decoder = zlib.decompressobj(zdict=dictionary)
assert decoder.decompress(encoded) + decoder.flush() == payload
assert decoder.eof

for compress, decompress, error in (
    (zlib.compress, zlib.decompress, zlib.error),
    (bz2.compress, bz2.decompress, OSError),
    (lzma.compress, lzma.decompress, lzma.LZMAError),
):
    encoded = compress(payload)
    assert decompress(encoded) == payload
    try:
        decompress(b"this is not a compressed stream")
    except error:
        pass
    else:
        raise AssertionError("malformed compression input was accepted")

for encoder, decoder in (
    (bz2.BZ2Compressor(), bz2.BZ2Decompressor()),
    (lzma.LZMACompressor(), lzma.LZMADecompressor()),
):
    encoded = encoder.compress(payload[:113]) + encoder.compress(payload[113:]) + encoder.flush()
    assert decoder.decompress(encoded + b"unused") == payload
    assert decoder.eof and decoder.unused_data == b"unused"

assert gzip.decompress(gzip.compress(payload, mtime=0)) == payload
print("Guest CPython compression: 27")
