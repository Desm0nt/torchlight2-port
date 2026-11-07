package com.xiaoboluo.d2r;

import java.io.ByteArrayInputStream;
import java.io.File;

// Desktop driver for the device converter (dm/android/.../Tl2ModConv.java), used by tools/tl2_mod_java_test.py:
//   java com.xiaoboluo.d2r.Tl2ModConvMain <mod> <out_overlay> <slot>
// PNG / JPG through ImageIO (the app uses BitmapFactory, unpremultiplied ARGB_8888).
final class Tl2ModConvMain {
    public static void main(String[] a) throws Exception {
        Tl2ModConv.ImageDecoder img = data -> {
            try {
                java.awt.image.BufferedImage im = javax.imageio.ImageIO.read(new ByteArrayInputStream(data));
                if (im == null) return null;
                int w = im.getWidth(), h = im.getHeight(); int[] p = new int[2 + w * h]; p[0] = w; p[1] = h;
                im.getRGB(0, 0, w, h, p, 2, w); return p;
            } catch (Exception e) { return null; }
        };
        Tl2ModConv.Result r = Tl2ModConv.convert(new File(a[0]), new File(a[1]), Integer.parseInt(a[2]), img);
        System.out.println(r.pack + ": " + r.files + " files, textures " + r.textures + " (converted " + r.converted + "), deleted " + r.deleted + ", failed " + r.failed.size());
        for (String f : r.failed) System.out.println("  failed " + f);
    }
}
