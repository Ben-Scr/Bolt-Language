using System.IO;

public static class Program {
    public static void Main() {
        byte[] height = File.ReadAllBytes("input.bin");
        int width = 2048;
        int rows = height.Length / width;
        byte[] mask = new byte[height.Length];
        int slopeLimit = 32;
        for (int y = 1; y < rows - 1; y++) {
            for (int x = 1; x < width - 1; x++) {
                int p = y * width + x;
                int dx = (int)height[p + 1] - (int)height[p - 1];
                int dy = (int)height[p + width] - (int)height[p - width];
                int slope = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                int material = height[p] < 64 ? 1 : (slope > slopeLimit ? 3 : 2);
                mask[p] = (byte)material;
            }
        }
        File.WriteAllBytes("result.bin", mask);
    }
}
