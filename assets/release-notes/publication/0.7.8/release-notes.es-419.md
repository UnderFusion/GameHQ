# GameHQ 0.7.8 (2026-09-22)

## Lo más destacado

- La superposición ahora funciona de forma mucho más confiable sobre juegos en modo sin bordes. Aparece por encima del juego, recibe la entrada del control para navegar por la superposición en las rutas de GameInput compatibles y el juego sigue visible. Se comprobó con un DualSense conectado por cable en Indiana Jones and the Great Circle en modo sin bordes.
- Navegar por la superposición ya no controla el juego que está debajo en las rutas de GameInput compatibles. Al cerrar la superposición, GameHQ espera a que sueltes los botones que tengas presionados antes de devolverle el control al juego.
- Los clips de repetición son más seguros: guardar varias veces seguidas nunca sobrescribe un clip existente, y puedes guardar un clip justo después de que empieza la grabación.
- Nuevos preajustes de asignaciones: crea, edita y asigna configuraciones de botones para cada control y cada juego.
- GameHQ recuerda dónde te quedaste: la última página, la categoría de Configuración, el filtro de la galería y la categoría de la superposición de cada juego.

## Superposición

- La superposición puede tomar el foco del control y seguir mostrándose sobre un juego visible en modo sin bordes, sin minimizarlo.
- En las rutas de GameInput compatibles, navegar por la superposición ya no controla también el juego.
- Al cerrar la superposición, se espera un momento a que los botones presionados y las palancas vuelvan a reposo antes de devolverle el control al juego, para que una entrada sostenida no pase al juego.
- Abrir y cerrar la superposición es más rápido y confiable, y una pulsación rápida repetida ya no la cierra justo después de abrirse. Se respetan Alt-Tab y los demás cambios intencionales a otra aplicación.
- La superposición sigue al juego correcto cuando este vuelve a crear su ventana, la pierde o pierde el foco por un momento. Se cierra correctamente cuando otra aplicación pasa a primer plano.
- La superposición se vuelve a abrir en la captura de pantalla o el clip que seleccionaste por última vez. Después de una captura nueva, empieza en el elemento más reciente.
- Los clips empiezan a reproducirse en la superposición sin que la vista previa desaparezca por un instante.
- El fondo oscurecido ahora aparece junto con los menús de la superposición, en lugar de aparecer gradualmente después.
- Las indicaciones de controles de la parte inferior ahora tienen un pequeño fondo que sigue tu ajuste de oscurecimiento de la superposición, para que se puedan leer bien sobre el juego.
- El juego que estás jugando ahora se resalta correctamente al desplazarte por la barra lateral.
- La navegación con el control funciona mejor en los menús de la superposición, la galería y la reproducción de video, y los botones de navegación se mantienen separados de los atajos de captura.

## Captura y repetición

- Al guardar varias veces seguidas, cada archivo recibe un nombre único en lugar de sobrescribir un clip existente. Una exportación fallida nunca elimina clips anteriores, y las miniaturas siempre corresponden al clip correcto.
- Al guardar una repetición se usa lo grabado hasta ese momento, aunque todavía no se complete la duración del búfer que configuraste.
- Una exportación de repetición conserva la grabación que necesita aunque cambies de juego o el búfer se reinicie, y GameHQ espera a que termine una exportación en curso antes de cerrarse.
- Las capturas de pantalla y los guardados de repeticiones se confirman al instante y después muestran claramente si se guardaron o fallaron. Las notificaciones se actualizan sin duplicarse y solo se muestra una cantidad limitada a la vez.
- Las capturas fallidas u omitidas ahora explican el motivo, incluso con las notificaciones de éxito desactivadas.
- El estado de la repetición ahora indica si la grabación realmente empezó y si hay grabación utilizable, con mensajes claros cuando el búfer se está iniciando, está vacío o hay una exportación en curso.
- Las sesiones de repetición manuales ahora se mantienen después de cambiar la configuración y al tomar capturas de pantalla HDR al mismo tiempo. Una sesión manual que inicias pero no llegas a usar se desactiva después de un tiempo.
- Guardar una repetición ya no detiene la captura mientras se crea la miniatura, y las capturas de pantalla tomadas al mismo tiempo crean su carpeta de forma más confiable.
- Nueva opción para el borde amarillo de captura de Windows, con información más clara sobre permisos y compatibilidad del sistema. La grabación sigue funcionando cuando Windows no puede ocultar el borde.

## Controles y entrada

- Detección de controles y enrutamiento de la entrada más confiables cuando hay varios controles o fuentes de entrada conectados.
- Al cambiar de fuente de entrada, reconectar controles y hacer seguimiento de los botones sostenidos, ahora se evitan pulsaciones perdidas, pulsaciones duplicadas y entradas trabadas.
- Se corrigieron las pulsaciones del botón PS que no se detectaban y las pulsaciones repetidas con retraso que podían volver a abrir la superposición justo después de cerrarla.
- Los gatillos, las pulsaciones de las palancas y los demás botones ahora se comportan igual en todas las fuentes de entrada compatibles, así que las asignaciones funcionan de forma más predecible.
- Los controles que usan GameInput siguen funcionando con normalidad, incluido el aislamiento de la superposición, cuando Windows no puede ofrecer la compatibilidad opcional con los botones Guide/Share.
- El control que estás editando en Configuración sigue seleccionado cuando se activa otro control.

## Preajustes de asignaciones

- Nueva biblioteca de preajustes para configuraciones de botones, donde puedes crear, cambiar el nombre, duplicar, editar y eliminar preajustes.
- Puedes asignar preajustes a controles y juegos, con una opción alternativa y selección automática para el juego que estás ejecutando.
- Tus asignaciones personalizadas pasan automáticamente al sistema de preajustes, y los datos originales se conservan como copia de seguridad.
- El cambio de preajuste tiene en cuenta los botones sostenidos y los gestos en curso, para que cambiar no active acciones por accidente.
- La interfaz distingue entre el preajuste asignado y el que estás editando, protege los cambios sin guardar y te permite crear una copia para un solo control.
- Al eliminar un preajuste en uso, primero se te pide un reemplazo o una opción alternativa, y los cambios en preajustes compartidos se indican claramente.

## Interfaz, configuración y sonido

- GameHQ se vuelve a abrir en la última página, categoría de Configuración y filtro de galería que usaste. La superposición recuerda su última categoría por separado para cada juego.
- La ventana ahora se restaura correctamente en monitores ubicados a la izquierda o por encima de la pantalla principal. Una ventana que se abriría completamente fuera de la pantalla se mueve de vuelta a una pantalla conectada.
- La ventana principal y la superposición tienen cada una su propia escala, de 100 % a 200 %, que se conserva entre reinicios. Las ventanas pequeñas se organizan mejor.
- Los sonidos de captura son más fuertes y se distinguen mejor, y tienen su propio control de volumen y vista previa. El volumen de la interfaz y de la captura puede llegar a 300 %.
- Cambiar la configuración de notificaciones, sonido, borde de captura o sesiones manuales ya no descarta tu búfer de repetición.
- Al abrir la galería se mantiene su filtro guardado. Si el filtro guardado corresponde a un juego que ya no existe, se restablece de forma segura. Las categorías de Configuración se mantienen aunque cambie su orden.
- Se corrigieron el contenido en blanco y los problemas de elementos superpuestos al cambiar la escala de la interfaz, y los menús ahora aparecen por encima del contenido principal.
- Los controles de asignación muestran cuánto dura una pulsación prolongada y explican cómo funcionan los gestos de pulsación prolongada.

## Actualizaciones, idiomas y diagnóstico

- Las notas de actualización aparecen en el idioma que elegiste, se muestran en inglés cuando no hay una traducción disponible y se pueden leer sin conexión una vez cargadas.
- Las notas de actualización pueden incluir un enlace opcional a la versión en GitHub, al que puedes acceder con el control.
- Se ampliaron las traducciones en todos los idiomas compatibles para los nuevos mensajes de captura, sonido, borde, pulsación prolongada, preajustes y foco.
- El diagnóstico copiado ahora incluye las asignaciones activas, la selección de preajuste, los cambios de fuente de entrada y el estado del foco de la superposición, con los identificadores de dispositivo anonimizados.
- El diagnóstico de captura sigue cada solicitud desde que presionas el botón hasta que se guarda el archivo, lo que facilita encontrar problemas. Los problemas de sonido se informan con una advertencia clara.

## Limitaciones conocidas

- Que la superposición aísle el control del juego depende del juego y de cómo lee el control. Un DualSense conectado por cable con GameInput está bien probado. No está garantizado con XInput, Raw Input, HID directo, Steam Input ni controles virtuales.
- El cambio de control con DSX en ejecución solo está verificado en parte. Tú mantienes el control de tu configuración de DSX; GameHQ no instala ni administra drivers de controles virtuales ni de ocultación de dispositivos.
- Algunos juegos se pausan o reaccionan al perder el foco. Windows u otra aplicación de captura pueden mantener visible el borde amarillo de grabación.
- Si no hay una nota de la versión 0.7.8 traducida, GameHQ la muestra en inglés.
