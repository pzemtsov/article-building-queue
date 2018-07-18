
public class DualArrayAsyncIntQueue extends IntQueue
{
    private final int size;
    private final int [] buf1;
    private final int [] buf2;
    int x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11;
    int [] read_buf;
    int read_ptr;
    int y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12;
    int [] write_buf;
    int write_ptr;
    int z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12;
    volatile int read_limit = 0;
    
    public DualArrayAsyncIntQueue (int size)
    {
        this.size = size;
        buf1 = new int [size];
        buf2 = new int [size];
        read_buf = write_buf = buf1;
        read_ptr = write_ptr = 0;
        read_limit = 0;
    }
    
    @Override
    public int read ()
    {
        int lim;

        while ((lim = read_limit) == 0) {
            Thread.yield();
        }
        int r = read_ptr;
        int [] rb = read_buf;
        int elem = rb[r++];
        if (r == lim) {
            read_limit = 0;
            read_buf = rb == buf1 ? buf2 : buf1;
            r = 0;
        }
        read_ptr = r;
        return elem;
    }
    
    @Override
    public void write (int value)
    {
        int [] wb = write_buf;
        int w = write_ptr;
        if (w < size) {
            wb[w++] = value;
        }
        if (read_limit == 0) {
            read_limit = w;
            write_buf = wb == buf1 ? buf2 : buf1;
            w = 0;
        }
        write_ptr = w;
    }
}
