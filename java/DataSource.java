
public class DataSource extends Thread
{
    private final IntQueue queue;
    private final int interval;
    
    public DataSource (IntQueue queue, int interval)
    {
        super ("DataSource");
        this.queue = queue;
        this.interval = interval;
    }

    double sum = 0;
    
    @Override
    public void run ()
    {
        int seq = 0;
        while (true)  {
            queue.write(seq++);
            sum = 0;
            for (int i = 1; i <= interval; i++) {
                 sum += (double) i * (double) i;
            }
        }
    }
}
