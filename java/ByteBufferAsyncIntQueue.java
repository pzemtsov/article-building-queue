import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;

public class ByteBufferAsyncIntQueue extends IntQueue
{
    final int size;
    final IntBuffer buf;
    final IntBuffer read_limit_buf;
    private int read_offset = 0;
    private int write_offset = 0;
    private int read_ptr = 0;
    private int write_ptr = 0;
    private int read_limit = 0;
    
    public ByteBufferAsyncIntQueue (int size)
    {
        this.size = size;
        buf = ByteBuffer.allocateDirect (size * 2 * 4).order (ByteOrder.nativeOrder ()).asIntBuffer ();
        read_limit_buf = ByteBuffer.allocateDirect (4).order (ByteOrder.nativeOrder ()).asIntBuffer ();
        read_ptr = write_ptr = 0;
        read_limit = 0;
    }
    
    @Override
    public int read ()
    {
        if (read_limit == 0) {
            while ((read_limit = read_limit_buf.get (0)) == 0) {
                Thread.yield();
            }
        }
        int r = read_ptr;
        int elem = buf.get (read_offset + r++);
        if (r == read_limit) {
            read_limit_buf.put (0, 0);
            read_limit = 0;
            read_offset ^= size;
            r = 0;
        }
        read_ptr = r;
        return elem;
    }
    
    @Override
    public void write (int value)
    {
        int w = write_ptr;
        if (w < size) {
            buf.put (write_offset + w++, value);
        }
        if (read_limit_buf.get (0) == 0) {
            read_limit_buf.put (0, w);
            write_offset ^= size;
            w = 0;
        }
        write_ptr = w;
    }
}
