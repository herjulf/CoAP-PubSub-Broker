package resources;

import org.eclipse.californium.core.CoapResource;
import org.eclipse.californium.core.server.resources.CoapExchange;

public class TemperatureResource extends CoapResource {
    public TemperatureResource(String name) {
        this(name, true);
    }
    
    public TemperatureResource(String name, boolean visible) {
        super(name, visible);
    }
    
    @Override
    public void handleGET(CoapExchange exchange) {
        // default max-age is 60 seconds
        // exchange.setMaxAge(60);
        exchange.respond("temperature: " + getValue()); // add response code
    }
    
    public String getValue() {
        return "19";
    }
}
