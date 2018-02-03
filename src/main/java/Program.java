
import java.util.LinkedList;
import java.util.List;
import org.eclipse.californium.core.CoapResource;
import resources.*;

public class Program {
    private static final int SERVER_PORT = 5683;
    
    public static void main(String[] args) {
        List<CoapResource> resources = new LinkedList<>();
        // Read from file/database?
        resources.add(new TemperatureResource("temperature"));
        
        Broker broker = new Broker(SERVER_PORT);
        for (CoapResource cr : resources)
            broker.add(cr);
        broker.start();
    }
}
