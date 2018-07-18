
public class Reporter extends Thread
{
    private final IntQueue report_queue;
    
    public Reporter (IntQueue report_queue)
    {
        super ("DataSink");
        this.report_queue = report_queue;
    }

    @Override
    public void run ()
    {
        long start = System.nanoTime ();
        long current_start = start;

        while (true) {
            int lost = report_queue.read();
            long now = System.nanoTime ();
            
            long since_start = now - start;
            long since_last = now - current_start;

            System.out.printf("%5.1f; write: %4d ns/elem; read: %4d ns/elem; received: %8d; lost: %8d\n"
                , since_start * 1.0E-9
                , since_last/ (DataSink.REPORT_INTERVAL + lost)
                , since_last/ DataSink.REPORT_INTERVAL
                , DataSink.REPORT_INTERVAL
                , lost
            );
            current_start = now;
        }
    }

}
