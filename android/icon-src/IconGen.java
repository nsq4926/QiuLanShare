import javax.imageio.ImageIO;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.geom.Ellipse2D;
import java.awt.image.BufferedImage;
import java.io.File;

/**
 * 图标生成器：从 1024 全出血方图生成各密度 mipmap + adaptive 图标前景。
 *
 * 为什么用 JDK 而不是 PIL/ImageMagick：这台机器上两者都没有，JDK 21 自带 ImageIO 与 Graphics2D。
 * 运行：`java -Djava.awt.headless=true IconGen.java <src.png> <resDir> [previewOut.png]`
 *
 * ## 上一版错在哪（真机看到"箭头太大、顶到边"）
 *
 * 直接把整张方图当作 adaptive **前景**层。但 adaptive 图标的画布是 108dp，系统只会露出中间
 * 的圆形/圆角方形遮罩区（约 66–72dp），即外圈 18dp 被裁掉。于是：
 *   ① 箭头被放大 1.5 倍并被边缘裁切 → "箭头太大、不协调"；
 *   ② 图里的渐变背景跟着被裁，和 `background` 那层纯色对不上 → 边缘出现色差。
 *
 * ## 现在的做法
 *
 *  - 把箭头从图里**抠出来**（按亮度反解 alpha，见 `extractArrows`），做成透明前景；
 *  - 前景里的箭头缩到画布的 42%，稳落在 66dp 安全圆内（占圆直径约 69%）；
 *  - 背景层用图里真实的渐变端点色，写成 shape 渐变（`ic_launcher_bg.xml`）；
 *  - 另外输出一张**圆形遮罩预览图**，用于肉眼验收，不必装到机器上才知道对不对。
 */
public final class IconGen {

    /** 箭头宽度占 108dp 画布的比例。0.42 → 45dp，落在 66dp 安全圆内约 69% */
    private static final double FG_ARROW_RATIO = 0.42;

    /** 传统图标（API < 26）圆角半径占边长的比例，避免在不会被遮罩的桌面上是个死方块 */
    private static final double LEGACY_CORNER = 0.20;

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: IconGen <source.png> <resDir> [previewOut.png]");
            System.exit(2);
        }
        File src = new File(args[0]);
        File res = new File(args[1]);
        BufferedImage img = ImageIO.read(src);
        if (img == null) {
            System.err.println("cannot read image: " + src);
            System.exit(3);
        }
        int w = img.getWidth(), h = img.getHeight();

        // ---- 1. 采样四角取背景渐变端点（箭头在中间，四角是纯渐变）
        int p = Math.max(4, w / 32);
        int[] c00 = patchAvg(img, p / 2, p / 2, p);
        int[] c10 = patchAvg(img, w - 1 - p / 2, p / 2, p);
        int[] c01 = patchAvg(img, p / 2, h - 1 - p / 2, p);
        int[] c11 = patchAvg(img, w - 1 - p / 2, h - 1 - p / 2, p);
        System.out.printf("bg corners: tl=#%06X tr=#%06X bl=#%06X br=#%06X%n",
            c00[0] << 16 | c00[1] << 8 | c00[2],
            c10[0] << 16 | c10[1] << 8 | c10[2],
            c01[0] << 16 | c01[1] << 8 | c01[2],
            c11[0] << 16 | c11[1] << 8 | c11[2]);

        // ---- 2. 抠出箭头（alpha = 该像素相对"渐变背景"的白色混入比例）
        BufferedImage arrows = extractArrows(img, c00, c10, c01, c11);
        int[] box = alphaBox(arrows);
        if (box == null) {
            System.err.println("no foreground art found (all transparent?)");
            System.exit(4);
        }
        System.out.printf("arrow bbox: x=%d..%d y=%d..%d (%dx%d)%n",
            box[0], box[2], box[1], box[3], box[2] - box[0], box[3] - box[1]);

        // ---- 3. 传统 mipmap：整张图（含渐变）+ 圆角
        String[] buckets = {"mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi"};
        int[] sizes = {48, 72, 96, 144, 192};
        for (int i = 0; i < buckets.length; i++) {
            File dir = new File(res, "mipmap-" + buckets[i]);
            if (!dir.exists() && !dir.mkdirs()) throw new IllegalStateException("cannot create " + dir);
            BufferedImage out = legacyIcon(img, sizes[i], false);
            ImageIO.write(out, "png", new File(dir, "ic_launcher.png"));
            // round 版必须是正圆：API < 26 的桌面会直接拿它当图标，用圆角方块当圆图标会露角
            ImageIO.write(legacyIcon(img, sizes[i], true), "png", new File(dir, "ic_launcher_round.png"));
            System.out.println("mipmap-" + buckets[i] + " " + sizes[i] + "px");
        }

        // ---- 4. adaptive 前景（108dp 画布 = 432px @xxxhdpi，箭头居中缩到 42%）
        File drawable = new File(res, "drawable");
        if (!drawable.exists() && !drawable.mkdirs()) throw new IllegalStateException("cannot create " + drawable);
        BufferedImage fg = adaptiveForeground(arrows, box, 432);
        ImageIO.write(fg, "png", new File(drawable, "ic_launcher_foreground.png"));
        ImageIO.write(fg, "png", new File(drawable, "ic_launcher_monochrome.png"));
        System.out.println("adaptive foreground 432px (arrow " + (int) (432 * FG_ARROW_RATIO) + "px)");

        // ---- 5. 预览：模拟系统的圆形遮罩，肉眼验收用
        if (args.length >= 3) {
            File out = new File(args[2]);
            BufferedImage prev = new BufferedImage(432, 432, BufferedImage.TYPE_INT_ARGB);
            Graphics2D g = prev.createGraphics();
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
            g.setClip(new Ellipse2D.Double(0, 0, 432, 432));
            g.drawImage(background(w, h, c00, c10, c01, c11, 432), 0, 0, null);
            g.drawImage(fg, 0, 0, null);
            g.dispose();
            ImageIO.write(prev, "png", out);
            System.out.println("preview -> " + out);
        }
        System.out.println("done");
    }

    /** 传统图标：整张图缩放到目标尺寸，按圆角方块（或正圆）裁切 */
    private static BufferedImage legacyIcon(BufferedImage src, int size, boolean round) {
        BufferedImage out = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = out.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        if (round) {
            g.setClip(new Ellipse2D.Double(0, 0, size, size));
        } else {
            double r = size * LEGACY_CORNER;
            g.setClip(new java.awt.geom.RoundRectangle2D.Double(0, 0, size, size, r * 2, r * 2));
        }
        g.drawImage(scale(src, size, size), 0, 0, null);
        g.dispose();
        return out;
    }

    /** adaptive 前景：透明画布，箭头按 bbox 居中缩放到 ratio*size */
    private static BufferedImage adaptiveForeground(BufferedImage arrows, int[] box, int size) {
        BufferedImage out = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        int bw = box[2] - box[0], bh = box[3] - box[1];
        double target = size * FG_ARROW_RATIO;
        double k = target / Math.max(bw, bh);
        int dw = (int) Math.round(bw * k), dh = (int) Math.round(bh * k);
        BufferedImage art = new BufferedImage(dw, dh, BufferedImage.TYPE_INT_ARGB);
        Graphics2D ag = art.createGraphics();
        ag.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        ag.drawImage(arrows.getSubimage(box[0], box[1], bw, bh), 0, 0, dw, dh, null);
        ag.dispose();

        Graphics2D g = out.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        g.drawImage(art, (size - dw) / 2, (size - dh) / 2, null);
        g.dispose();
        return out;
    }

    /**
     * 抠图。
     *
     * 背景不是纯色，是沿对角线变化的渐变。做法分三步：
     *  ① 用最小二乘在**低饱和像素**（必是背景）上拟合平面 I_c = a + b·x + c·y，得到背景模型；
     *  ② 每像素解出白色混入比例 t = (I - B) / (255 - B)，取三通道最大值当 alpha；
     *  ③ 只保留面积足够大的连通域，丢掉拟合残差造成的白雾小块。
     *
     * 第 ③ 步是必需的：第一版只用双线性角点模型 + 低阈值，右下角留下一片斜纹残影
     * （预览图里肉眼可见），真机上就是脏边。
     */
    private static BufferedImage extractArrows(BufferedImage img, int[] c00, int[] c10, int[] c01, int[] c11) {
        int w = img.getWidth(), h = img.getHeight();
        double[][] plane = fitBackground(img);
        double[][] alpha = new double[h][w];
        double tMaxBg = 0; // 诊断：背景区域上"看起来像前景"的最大值
        for (int y = 0; y < h; y++) {
            double fy = h <= 1 ? 0 : (double) y / (h - 1);
            for (int x = 0; x < w; x++) {
                double fx = w <= 1 ? 0 : (double) x / (w - 1);
                int rgb = img.getRGB(x, y);
                int[] ch = {(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF};
                double t = 0;
                for (int i = 0; i < 3; i++) {
                    double b = plane[i][0] + plane[i][1] * fx + plane[i][2] * fy;
                    double denom = 255.0 - b;
                    if (denom < 1.0) continue;
                    t = Math.max(t, (ch[i] - b) / denom);
                }
                boolean isBg = isBackground(ch);
                if (isBg) tMaxBg = Math.max(tMaxBg, t);
                double a = t <= T_FLOOR ? 0 : Math.min(1, (t - T_FLOOR) / (1 - T_FLOOR));
                alpha[y][x] = a;
            }
        }
        System.out.printf("bg fit residual: max front-like t on bg = %.3f (floor=%.2f)%n", tMaxBg, T_FLOOR);

        // 只留大块：箭头可能被分成两个独立图形，故按面积阈值而不是"最大块"
        boolean[][] keep = keepLargeComponents(alpha, w, h, (int) (w * (long) h / 200));

        BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (!keep[y][x] || alpha[y][x] <= 0) continue;
                int a8 = (int) Math.round(alpha[y][x] * 255);
                out.setRGB(x, y, (a8 << 24) | 0xFFFFFF); // 前景恒为白色，颜色交给背景层/系统着色
            }
        }
        return out;
    }

    /**
     * 是否"确定是渐变背景"。
     *
     * 不能用"低饱和"来判：白色箭头的饱和度同样是 0，会把前景混进背景拟合里，
     * 结果是拟合出的平面偏亮、抠图只剩一角（第一版就这样，bbox 只剩右下小块）。
     * 渐变底是高饱和的蓝（s≈0.5–0.65），所以**高饱和**才是背景的判据。
     */
    private static boolean isBackground(int[] ch) {
        int max = Math.max(ch[0], Math.max(ch[1], ch[2]));
        return saturation(ch) >= 0.35 && max < 250;
    }

    /** 阈值下限：低于它的算背景（消掉拟合残差） */
    private static final double T_FLOOR = 0.30;

    private static double saturation(int[] ch) {
        int max = Math.max(ch[0], Math.max(ch[1], ch[2]));
        int min = Math.min(ch[0], Math.min(ch[1], ch[2]));
        return max == 0 ? 0 : (double) (max - min) / max;
    }

    /** 在低饱和像素上最小二乘拟合每个通道的平面（xn/yn 归一化到 0..1） */
    private static double[][] fitBackground(BufferedImage img) {
        double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        double[] si = new double[3], sxi = new double[3], syi = new double[3];
        for (int y = 0; y < img.getHeight(); y += 3) {
            double yn = img.getHeight() <= 1 ? 0 : (double) y / (img.getHeight() - 1);
            for (int x = 0; x < img.getWidth(); x += 3) {
                int rgb = img.getRGB(x, y);
                int[] ch = {(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF};
                if (!isBackground(ch)) continue; // 只看肯定是背景的样本（白箭头也是低饱和，不能算背景）
                double xn = img.getWidth() <= 1 ? 0 : (double) x / (img.getWidth() - 1);
                n++; sx += xn; sy += yn; sxx += xn * xn; syy += yn * yn; sxy += xn * yn;
                for (int i = 0; i < 3; i++) {
                    si[i] += ch[i];
                    sxi[i] += ch[i] * xn;
                    syi[i] += ch[i] * yn;
                }
            }
        }
        double[][] out = new double[3][3];
        if (n < 16) { // 退化：全透明图，模型无意义
            for (int i = 0; i < 3; i++) out[i] = new double[]{255, 0, 0};
            return out;
        }
        double[][] m = {{n, sx, sy}, {sx, sxx, sxy}, {sy, sxy, syy}};
        double det = det3(m);
        if (Math.abs(det) < 1e-9) {
            for (int i = 0; i < 3; i++) out[i] = new double[]{si[i] / n, 0, 0};
            return out;
        }
        for (int i = 0; i < 3; i++) {
            double[][] a = {{si[i], sx, sy}, {sxi[i], sxx, sxy}, {syi[i], sxy, syy}};
            double[][] b = {{n, si[i], sy}, {sx, sxi[i], sxy}, {sy, syi[i], syy}};
            double[][] d = {{n, sx, si[i]}, {sx, sxx, sxi[i]}, {sy, sxy, syi[i]}};
            out[i] = new double[]{det3(a) / det, det3(b) / det, det3(d) / det};
        }
        return out;
    }

    private static double det3(double[][] m) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }

    /** 连通域过滤：面积小于 minArea 的一律丢掉（4 邻接洪水填充） */
    private static boolean[][] keepLargeComponents(double[][] alpha, int w, int h, int minArea) {
        boolean[][] keep = new boolean[h][w];
        boolean[][] seen = new boolean[h][w];
        int[] stack = new int[w * h];
        for (int sy = 0; sy < h; sy++) {
            for (int sx = 0; sx < w; sx++) {
                if (seen[sy][sx] || alpha[sy][sx] <= 0.02) continue;
                int top = 0;
                stack[top++] = sy * w + sx;
                seen[sy][sx] = true;
                java.util.ArrayList<Integer> comp = new java.util.ArrayList<>();
                while (top > 0) {
                    int cur = stack[--top];
                    comp.add(cur);
                    int cy = cur / w, cx = cur % w;
                    int[][] nb = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
                    for (int[] p : nb) {
                        if (p[0] < 0 || p[1] < 0 || p[0] >= w || p[1] >= h) continue;
                        if (seen[p[1]][p[0]] || alpha[p[1]][p[0]] <= 0.02) continue;
                        seen[p[1]][p[0]] = true;
                        stack[top++] = p[1] * w + p[0];
                    }
                }
                if (comp.size() >= minArea) {
                    for (int idx : comp) keep[idx / w][idx % w] = true;
                }
            }
        }
        return keep;
    }

    private static double bilinear(int v00, int v10, int v01, int v11, double fx, double fy) {
        double top = v00 + (v10 - v00) * fx;
        double bot = v01 + (v11 - v01) * fx;
        return top + (bot - top) * fy;
    }

    /** 按四角模型铺满一张渐变（预览用；XML 里用 shape gradient 达到同样效果） */
    private static BufferedImage background(int sw, int sh, int[] c00, int[] c10, int[] c01, int[] c11, int size) {
        BufferedImage out = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        for (int y = 0; y < size; y++) {
            double fy = (double) y / (size - 1);
            for (int x = 0; x < size; x++) {
                double fx = (double) x / (size - 1);
                int r = (int) Math.round(bilinear(c00[0], c10[0], c01[0], c11[0], fx, fy));
                int g = (int) Math.round(bilinear(c00[1], c10[1], c01[1], c11[1], fx, fy));
                int b = (int) Math.round(bilinear(c00[2], c10[2], c01[2], c11[2], fx, fy));
                out.setRGB(x, y, 0xFF000000 | (r << 16) | (g << 8) | b);
            }
        }
        return out;
    }

    private static int[] alphaBox(BufferedImage img) {
        int minX = Integer.MAX_VALUE, minY = Integer.MAX_VALUE, maxX = -1, maxY = -1;
        for (int y = 0; y < img.getHeight(); y++) {
            for (int x = 0; x < img.getWidth(); x++) {
                if ((img.getRGB(x, y) >>> 24) > 16) {
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX < 0) return null;
        return new int[]{minX, minY, maxX + 1, maxY + 1};
    }

    private static int[] patchAvg(BufferedImage img, int cx, int cy, int r) {
        long[] sum = {0, 0, 0};
        int n = 0;
        for (int y = cy - r; y <= cy + r; y++) {
            for (int x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= img.getWidth() || y >= img.getHeight()) continue;
                int rgb = img.getRGB(x, y);
                sum[0] += (rgb >> 16) & 0xFF;
                sum[1] += (rgb >> 8) & 0xFF;
                sum[2] += rgb & 0xFF;
                n++;
            }
        }
        if (n == 0) return new int[]{255, 255, 255};
        return new int[]{(int) (sum[0] / n), (int) (sum[1] / n), (int) (sum[2] / n)};
    }

    private static BufferedImage scale(BufferedImage src, int w, int h) {
        BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = out.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        g.setRenderingHint(RenderingHints.KEY_RENDERING, RenderingHints.VALUE_RENDER_QUALITY);
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.drawImage(src, 0, 0, w, h, null);
        g.dispose();
        return out;
    }
}
