
import org.eclipse.californium.core.CoapResource;
import org.eclipse.californium.core.CoapServer;
import org.eclipse.californium.core.coap.CoAP.ResponseCode;
import org.eclipse.californium.core.server.resources.CoapExchange;
import org.eclipse.californium.core.server.resources.DiscoveryResource;
import org.eclipse.californium.core.server.resources.Resource;

// coap://127.0.0.1:5683

public class Broker extends CoapServer {
    private final Resource root = new RootResource();

    public Broker(final int... ports) {
        super(ports);
        
        CoapResource wellKnown = new CoapResource(".well-known");
        wellKnown.add(new DiscoveryResource(root));
        root.add(wellKnown);
    }
    
    @Override
    protected Resource createRoot() {
        return new RootResource();
    }
    
    private class RootResource extends CoapResource {
        public RootResource() {
            super("");
        }
        
        @Override
        public void handleGET(CoapExchange exchange) {
            exchange.respond(ResponseCode.CONTENT, "temperature");
        }
    }
}
