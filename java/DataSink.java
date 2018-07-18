
public class DataSink extends Thread
{
    private final IntQueue queue;
    private final IntQueue report_queue;
    public static final int REPORT_INTERVAL = 10000000;
    
    public DataSink (IntQueue queue, IntQueue report_queue)
    {
        super ("DataSink");
        this.queue = queue;
        this.report_queue = report_queue;
    }
    
    @Override
    public void run ()
    {
        int seq = 0;
        int lost_count = 0;
        int received_count = 0;
        while (true) {
            int elem = queue.read ();
            lost_count += elem - seq;
            seq = elem + 1;
            ++received_count;
            if (received_count == REPORT_INTERVAL) {
                report_queue.write(lost_count);
                lost_count = received_count = 0;
            }
        }
    }
}
