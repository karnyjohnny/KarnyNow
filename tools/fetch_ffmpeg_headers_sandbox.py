import urllib.request, tarfile, io, os, time

t0 = time.time()
url = ("https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/"
       "bf0e831606938f530326c9679591109407fa8d0d")
data = urllib.request.urlopen(url, timeout=240).read()
print("downloaded %.1f MB in %.1fs" % (len(data) / 1e6, time.time() - t0))

root = "FFmpeg-bf0e831606938f530326c9679591109407fa8d0d/"
want = ("libavcodec/", "libavutil/", "libswscale/")
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dest = os.path.join(here, "third_party", "ffmpeg-headers") + os.sep
n = 0
with tarfile.open(fileobj=io.BytesIO(data)) as tf:
    for m in tf.getmembers():
        if not m.name.startswith(root):
            continue
        rel = m.name[len(root):]
        if not rel.endswith(".h"):
            continue
        if not any(rel.startswith(w) for w in want):
            continue
        out = dest + rel
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(tf.extractfile(m).read())
        n += 1
print("extracted", n, "headers")

# configure-generated stand-ins (BOOTSTRAP step 4)
with open(dest + "libavutil/avconfig.h", "w") as f:
    f.write("#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
            "#define AV_HAVE_BIGENDIAN 0\n#endif\n")
with open(dest + "libavutil/ffversion.h", "w") as f:
    f.write("#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n"
            '#define FFMPEG_VERSION "N-127939-gbf0e831606"\n#endif\n')
print("stand-ins written")
