using System.IO;

public static class Program {
    static void Merge(int[] source, int[] dest, int[] times, int size, int width) {
        for (int start = 0; start < size; start += width * 2) {
            int mid = start + width;
            if (mid > size) mid = size;
            int end = mid + width;
            if (end > size) end = size;
            int left = start;
            int right = mid;
            for (int k = start; k < end; k++) {
                if (right == end || (left < mid && times[source[left]] <= times[source[right]])) {
                    dest[k] = source[left++];
                } else {
                    dest[k] = source[right++];
                }
            }
        }
    }

    public static void Main() {
        byte[] data = File.ReadAllBytes("input.bin");
        int length = data.Length;
        int size = length / 2;
        int[] times = new int[size];
        int[] order = new int[size];
        int[] scratch = new int[size];
        for (int i = 0; i < size; i++) {
            times[i] = data[2 * i] + data[2 * i + 1] * 256;
            order[i] = i;
        }
        int pass = 0;
        for (int width = 1; width < size; width *= 2) {
            if (pass % 2 == 0) Merge(order, scratch, times, size, width);
            else Merge(scratch, order, times, size, width);
            pass++;
        }
        byte[] report = new byte[size * 2];
        for (int i = 0; i < size; i++) {
            int id = 0;
            if (pass % 2 == 0) id = order[i];
            else id = scratch[i];
            report[i * 2] = (byte)(id & 255);
            report[i * 2 + 1] = (byte)(id >> 8);
        }
        File.WriteAllBytes("result.bin", report);
    }
}
