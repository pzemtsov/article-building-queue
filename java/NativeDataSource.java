import java.nio.IntBuffer;

public class NativeDataSource
{
    private final ByteBufferAsyncIntQueue queue;
    private final int interval;

    static
    {
        System.loadLibrary ("NativeDataSource");
        calibrate ();
    }

    public NativeDataSource (IntQueue queue, int interval)
    {
        this.queue = (ByteBufferAsyncIntQueue) queue;
        this.interval = interval;
    }
    
    public void start ()
    {
        System.out.println ("Starting native decoder");
        start (queue.buf, queue.size, queue.read_limit_buf, interval);
    }

    public static native void calibrate ();
    private static native void start (IntBuffer queue, int size, IntBuffer read_limit, int interval);
}
