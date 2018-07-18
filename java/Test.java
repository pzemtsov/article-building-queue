
public class Test
{
    public static void main (String [] args) throws Exception
    {
        String type = args.length == 0 ? "array" : args [0];
        int interval = args.length < 2  ? 0 : Integer.parseInt (args[1]);
        
        IntQueue report_queue = new DualArrayAsyncIntQueue (1000);
        new Reporter (report_queue).start ();
        IntQueue queue;
        switch (type) {
        case "array":
            queue  = new DualArrayAsyncIntQueue (1000000);
            break;
        case "buffer":
        case "native-buffer":
            queue  = new ByteBufferAsyncIntQueue (1000000);
            break;
        default:
            System.out.println ("Wrong queue type: " + type);
            return;
        }

        System.out.println ("Queue constructed: " + queue.getClass ());
        new DataSink (queue, report_queue).start ();
        
        switch (type) {
        case "array":
        case "buffer":
            new DataSource (queue, interval).start ();
            break;
        case "native-buffer":
            new NativeDataSource (queue, interval).start ();
            break;
        }
    }
}
